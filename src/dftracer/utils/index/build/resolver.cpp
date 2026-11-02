// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <cstdint>
#include <exception>
#include <mutex>
#include <string_view>

namespace dftracer::utils::index::build {

namespace {

using index::FileRef;
using index::Snapshot;
using index::store::IndexExtension;

struct PendingFile {
    std::size_t file_index;
    std::string file_path;
    std::string logical_path;
    std::uint64_t mtime;
    std::uint64_t size;
};

struct ResolveGroupInput {
    std::string index_path;
    std::vector<PendingFile> files;
    bool require_checkpoints;
    bool require_bloom;
    std::size_t checkpoint_size = 0;
    std::optional<ChunkIndexerConfig> bloom_config;
    std::string schema;
    index::schemas::dft::agg::AggregationConfig aggregation_config =
        index::schemas::dft::agg::standard_aggregation_config();
};

struct ResolveGroupOutput {
    std::vector<FileWorkItem> needs_checkpoint;
    std::vector<FileWorkItem> needs_bloom;
    std::vector<FileWorkItem> needs_aggregation;
    std::vector<ResolvedFile> cached;

    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us = 0;
    bool stale_detected = false;
    bool aggregation_stale = false;
    std::string index_path;
};

// The file's copy of `ext` was built, and with `params_hash`.
bool ready_with_params(const Snapshot& snapshot, const FileRef& file,
                       IndexExtension ext, std::uint64_t params_hash) {
    const auto state = index::extension_state(snapshot, file, ext);
    return state && state->ready && state->params_hash == params_hash;
}

// Whether the file's rows are in the tier, built with `params_hash`.
bool agg_current(const Snapshot& snapshot, const FileRef& file,
                 std::uint64_t params_hash) {
    return ready_with_params(snapshot, file, IndexExtension::Agg, params_hash);
}

// Paths the file's pruning tier indexed by value (bloom) or range (zonemap).
std::vector<std::string> indexed_paths(const Snapshot& snapshot,
                                       const FileRef& file) {
    std::vector<std::string> out;
    for (auto ext : {IndexExtension::Bloom, IndexExtension::Zonemap})
        for (auto& p : common::unwrap(index::paths(snapshot, file, ext)))
            out.emplace_back(p.data(), p.size());
    return out;
}

bool covers(const std::vector<std::string>& indexed,
            const std::vector<std::string>& fields) {
    for (const std::string& f : fields)
        if (std::find(indexed.begin(), indexed.end(), f) == indexed.end())
            return false;
    return true;
}

// The file's recorded schema is current and, when `wanted` is set, is it.
bool schema_current(const Snapshot& snapshot, const FileRef& file,
                    const std::string& wanted) {
    const auto recorded = common::unwrap(index::schema(snapshot, file));
    if (!recorded) return false;
    const std::string id(recorded->data(), recorded->size());
    if (!wanted.empty() && id != wanted) return false;
    const RecordSchema* schema = find_schema(id);
    if (!schema) return false;
    const auto state =
        index::extension_state(snapshot, file, IndexExtension::Profile);
    return state && state->params_hash == schema->params_hash();
}

// The tier extensions of the file that are missing or were built with other
// settings than `base` gives its schema.
index::store::ExtensionMask stale_tier(
    const Snapshot& snapshot, const FileRef& file,
    const std::optional<ChunkIndexerConfig>& base, const RecordSchema* schema) {
    index::store::ExtensionMask stale;
    for (auto ext : {IndexExtension::Stats, IndexExtension::Catalog,
                     IndexExtension::Metadata})
        if (!index::extension_current(snapshot, file, ext)) stale.add(ext);
    if (!base) {
        if (!stale.empty()) stale.add(index::store::PRUNING_EXTENSIONS);
        return stale;
    }
    const std::optional<ChunkIndexerConfig> config =
        schema ? for_schema(*base, *schema) : *base;
    for (auto ext : index::store::ALL_EXTENSIONS) {
        if (!index::store::PRUNING_EXTENSIONS.has(ext) ||
            !config->extensions.has(ext))
            continue;
        if (!ready_with_params(snapshot, file, ext, config->params_hash(ext)))
            stale.add(ext);
    }
    const bool paths = config->extensions.has(IndexExtension::Bloom) ||
                       config->extensions.has(IndexExtension::Zonemap);
    if (paths &&
        !covers(indexed_paths(snapshot, file), config->extra_dimensions)) {
        for (auto ext : {IndexExtension::Bloom, IndexExtension::Zonemap})
            if (config->extensions.has(ext)) stale.add(ext);
    }
    const auto registered = index::extensions::plugin_extensions();
    if (!registered.empty()) {
        const auto copies = common::unwrap(index::plugins(snapshot, file));
        for (const auto& ext : registered) {
            const index::PluginCopy* copy = nullptr;
            for (const auto& candidate : copies)
                if (std::string_view(candidate.name.data(),
                                     candidate.name.size()) == ext->name)
                    copy = &candidate;
            if (copy == nullptr ||
                !ext->current(index::store::ExtensionState{
                    copy->version, copy->params_hash, copy->ready})) {
                stale.add(IndexExtension::Plugin);
                break;
            }
        }
    }
    return stale;
}

// Files per parallel_for chunk of the fresh check.
constexpr std::int64_t DECIDE_GRAIN = 16;

// What the fresh check found for one file. Kind Aggregate is a current file
// whose tier rows must be built.
struct FileDecision {
    enum class Kind : std::uint8_t {
        NotIndexed,
        Stale,
        NeedsCheckpoint,
        Bloom,
        Aggregate,
        Cached
    };
    std::string schema;
    index::store::ExtensionMask tier;
    Kind kind = Kind::NotIndexed;
    bool agg_candidate = false;
    bool agg_current = false;
};

// Reads only. Safe to run for many files at once on one snapshot.
FileDecision decide(const Snapshot& snapshot, const PendingFile& f,
                    const ResolveGroupInput& input, std::uint64_t params) {
    FileDecision d;
    const auto file = snapshot.file(f.logical_path);
    if (!file) return d;

    // Stat-only staleness using mtime/size captured during the scan
    // (no extra metadata op). Rebuild if the source changed since it
    // was indexed, or the record predates mtime/size tracking.
    const auto& stored_stat = file->stat();
    if (stored_stat.mtime != f.mtime || stored_stat.size != f.size) {
        DFTRACER_UTILS_LOG_WARN(
            "Index stale for %s (source changed since indexing); "
            "rebuilding",
            f.file_path.c_str());
        d.kind = FileDecision::Kind::Stale;
        return d;
    }

    const bool has_checkpoints =
        index::extension_current(snapshot, *file, IndexExtension::Members) &&
        schema_current(snapshot, *file, input.schema);
    if (input.require_checkpoints && !has_checkpoints) {
        d.kind = FileDecision::Kind::NeedsCheckpoint;
        return d;
    }

    // A changed checkpoint size re-checkpoints the file. The build path
    // stores the requested size verbatim, so an exact compare suffices
    // (no file read). Zero on either side means "unspecified": skip.
    if (input.require_checkpoints && input.checkpoint_size != 0) {
        const auto metadata =
            common::unwrap(index::file_metadata(snapshot, *file));
        const std::uint64_t stored_ckpt =
            metadata ? metadata->checkpoint_size : 0;
        if (stored_ckpt != 0 && stored_ckpt != input.checkpoint_size) {
            d.kind = FileDecision::Kind::NeedsCheckpoint;
            return d;
        }
    }

    const auto recorded = common::unwrap(index::schema(snapshot, *file));
    if (recorded) d.schema.assign(recorded->data(), recorded->size());
    bool needs_agg = false;
    if (find_schema(d.schema)) {
        d.agg_candidate = true;
        d.agg_current = agg_current(snapshot, *file, params);
        needs_agg = !d.agg_current;
    }
    if (input.require_bloom) {
        d.tier = stale_tier(snapshot, *file, input.bloom_config,
                            find_schema(d.schema));
        if (!d.tier.empty()) {
            d.kind = FileDecision::Kind::Bloom;
            return d;
        }
    }
    d.kind =
        needs_agg ? FileDecision::Kind::Aggregate : FileDecision::Kind::Cached;
    return d;
}

ResolveGroupOutput resolve_group_sync(ResolveGroupInput input) {
    ResolveGroupOutput result;
    result.index_path = input.index_path;
    if (!input.index_path.empty()) load_index_schemas(input.index_path);

    if (input.index_path.empty() ||
        !common::unwrap(dstd::fs::exists(
            dstd::PathView(index::store_of(input.index_path))))) {
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), {}, {}});
        }
        return result;
    }

    try {
        const auto snapshot =
            common::unwrap(index::plan::open_snapshot(input.index_path));

        const std::uint64_t params = input.aggregation_config.params_hash();
        if (auto stored = common::unwrap(
                index::schemas::dft::agg::tier::read_config(snapshot))) {
            result.stored_time_interval_us = stored->time_interval_us;
            if (stored->params_hash != params) {
                result.aggregation_stale = true;
                auto same_interval = input.aggregation_config;
                same_interval.time_interval_us = stored->time_interval_us;
                result.needs_augmentation =
                    same_interval.params_hash() == stored->params_hash;
            }
        } else {
            // Partials with no config record may have keys in another layout,
            // so they are never read and the tier is built again.
            for (const auto& file : common::unwrap(snapshot.files())) {
                if (index::extension_state(snapshot, file,
                                           IndexExtension::Agg)) {
                    result.aggregation_stale = true;
                    break;
                }
            }
        }
        std::vector<FileDecision> decisions(input.files.size());
        std::exception_ptr failure;
        std::mutex failure_mutex;
        default_runtime().parallel_for(
            static_cast<std::int64_t>(input.files.size()), DECIDE_GRAIN,
            [&](std::int64_t begin, std::int64_t end) {
                try {
                    for (std::int64_t i = begin; i < end; ++i) {
                        const auto at = static_cast<std::size_t>(i);
                        decisions[at] =
                            decide(snapshot, input.files[at], input, params);
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
            });
        if (failure) std::rethrow_exception(failure);

        std::vector<std::pair<FileWorkItem, bool>> agg_candidates;
        for (std::size_t i = 0; i < input.files.size(); ++i) {
            PendingFile& f = input.files[i];
            FileDecision& d = decisions[i];
            switch (d.kind) {
                case FileDecision::Kind::Stale:
                    result.stale_detected = true;
                    [[fallthrough]];
                case FileDecision::Kind::NotIndexed:
                case FileDecision::Kind::NeedsCheckpoint:
                    result.needs_checkpoint.push_back(FileWorkItem{
                        f.file_index, std::move(f.file_path), {}, {}});
                    continue;
                default:
                    break;
            }
            if (d.agg_candidate)
                agg_candidates.emplace_back(
                    FileWorkItem{f.file_index, f.file_path, {}, d.schema},
                    d.agg_current);
            if (d.kind == FileDecision::Kind::Bloom) {
                result.needs_bloom.push_back(
                    FileWorkItem{f.file_index, std::move(f.file_path), d.tier,
                                 std::move(d.schema)});
            } else if (d.kind == FileDecision::Kind::Cached) {
                result.cached.push_back(ResolvedFile{
                    f.file_index, std::move(f.file_path), std::move(d.schema)});
            }
        }
        // A file already current must be aggregated again when the tier is
        // cleared; it then is not cached.
        for (auto& [item, current] : agg_candidates) {
            if (current && !result.aggregation_stale) continue;
            if (current)
                std::erase_if(result.cached, [&](const ResolvedFile& c) {
                    return c.file_index == item.file_index;
                });
            result.needs_aggregation.push_back(std::move(item));
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN(
            "Index resolve failed (%s); falling back to full rebuild",
            e.what());
        // The tier may be what failed to read, so it is cleared.
        result.aggregation_stale = true;
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), {}, {}});
        }
    }

    return result;
}

}  // namespace

std::vector<utilities::filesystem::FileEntry> scanned_entries(
    const ResolverResult& result) {
    std::vector<utilities::filesystem::FileEntry> out;
    out.reserve(result.all_files.size());
    for (std::size_t i = 0; i < result.all_files.size(); ++i) {
        utilities::filesystem::FileEntry entry;
        entry.path = dstd::Path(result.all_files[i]);
        entry.size = result.all_file_sizes[i];
        entry.mtime = result.all_file_mtimes[i];
        entry.is_regular_file = true;
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<utilities::filesystem::FileEntry> scanned_entries(
    const ResolverResult& result, const std::vector<FileWorkItem>& items) {
    std::vector<utilities::filesystem::FileEntry> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        utilities::filesystem::FileEntry entry;
        entry.path = dstd::Path(item.file_path);
        entry.size = result.all_file_sizes[item.file_index];
        entry.mtime = result.all_file_mtimes[item.file_index];
        entry.is_regular_file = true;
        out.push_back(std::move(entry));
    }
    return out;
}

coro::CoroTask<ResolverResult> Resolver::operator()(
    CoroScope& ctx, const ResolverInput& input) const {
    DFTRACER_UTILS_TRACE_SCOPE("resolve index");
    ResolverResult result;

    if (!input.entries.empty()) {
        result.all_files.reserve(input.entries.size());
        result.all_file_sizes.reserve(input.entries.size());
        result.all_file_mtimes.reserve(input.entries.size());
        for (const auto& entry : input.entries) {
            result.all_files.push_back(std::string(entry.path.str()));
            result.all_file_sizes.push_back(entry.size);
            result.all_file_mtimes.push_back(entry.mtime);
        }
    } else if (!input.directory.empty()) {
        utilities::filesystem::PatternDirectoryScannerUtilityInput scan_input{
            input.directory, utilities::filesystem::trace_file_patterns(),
            true};
        std::vector<utilities::filesystem::FileEntry> matched;
        matched = co_await scanner_(ctx, scan_input);
        result.all_files.reserve(matched.size());
        result.all_file_sizes.reserve(matched.size());
        result.all_file_mtimes.reserve(matched.size());
        for (const auto& entry : matched) {
            result.all_files.push_back(std::string(entry.path.str()));
            result.all_file_sizes.push_back(entry.size);
            result.all_file_mtimes.push_back(entry.mtime);
        }
    } else {
        result.all_files = input.files;
        result.all_file_sizes.assign(input.files.size(), 0);
        result.all_file_mtimes.assign(input.files.size(), 0);
        for (std::size_t i = 0; i < input.files.size(); ++i) {
            const utilities::filesystem::FileEntry entry(input.files[i]);
            result.all_file_sizes[i] = entry.size;
            result.all_file_mtimes[i] = entry.mtime;
        }
    }

    if (result.all_files.empty()) {
        co_return result;
    }

    result.index_path = trace::internal::determine_index_path(
        result.all_files.front(), input.index_dir);

    dstd::StringViewMap<std::vector<PendingFile>> groups;
    for (std::size_t i = 0; i < result.all_files.size(); ++i) {
        const auto& file_path = result.all_files[i];
        auto idx_path =
            trace::internal::determine_index_path(file_path, input.index_dir);
        auto logical = index::store::internal::get_logical_path(file_path);
        groups[idx_path].push_back(PendingFile{i, file_path, std::move(logical),
                                               result.all_file_mtimes[i],
                                               result.all_file_sizes[i]});
    }

    std::vector<ResolveGroupOutput> outputs;
    outputs.reserve(groups.size());

    if (groups.size() > 1) {
        std::vector<coro::SpawnFuture<ResolveGroupOutput>> futures;
        futures.reserve(groups.size());

        for (auto& [idx_path, files] : groups) {
            ResolveGroupInput group_input;
            group_input.index_path = idx_path;
            group_input.files = std::move(files);
            group_input.require_checkpoints = input.require_checkpoints;
            group_input.require_bloom = input.require_bloom;
            group_input.checkpoint_size = input.checkpoint_size;
            group_input.bloom_config = input.bloom_config;
            group_input.schema = input.schema;
            group_input.aggregation_config = input.aggregation_config;

            futures.push_back(ctx.spawn(
                [gi = std::move(group_input)](
                    CoroScope&) mutable -> coro::CoroTask<ResolveGroupOutput> {
                    co_return resolve_group_sync(std::move(gi));
                }));
        }

        for (auto& f : futures) {
            outputs.push_back(co_await f);
        }
    } else {
        for (auto& [idx_path, files] : groups) {
            ResolveGroupInput group_input;
            group_input.index_path = idx_path;
            group_input.files = std::move(files);
            group_input.require_checkpoints = input.require_checkpoints;
            group_input.require_bloom = input.require_bloom;
            group_input.checkpoint_size = input.checkpoint_size;
            group_input.bloom_config = input.bloom_config;
            group_input.schema = input.schema;
            group_input.aggregation_config = input.aggregation_config;

            outputs.push_back(resolve_group_sync(std::move(group_input)));
        }
    }

    for (auto& out : outputs) {
        for (auto& item : out.needs_checkpoint) {
            result.needs_checkpoint.push_back(std::move(item));
        }
        for (auto& item : out.needs_bloom) {
            result.needs_bloom.push_back(std::move(item));
        }
        for (auto& item : out.needs_aggregation) {
            result.needs_aggregation.push_back(std::move(item));
        }
        for (auto& item : out.cached) {
            result.cached.push_back(std::move(item));
        }
        // All groups share the same global config, so this is not a conflict.
        if (out.needs_augmentation) {
            result.needs_augmentation = true;
        }
        if (out.stored_time_interval_us != 0) {
            result.stored_time_interval_us = out.stored_time_interval_us;
        }
        if (out.stale_detected) {
            result.stale_detected = true;
        }
        if (out.aggregation_stale)
            result.stale_aggregation_roots.push_back(out.index_path);
    }

    DFTRACER_UTILS_LOG_INFO(
        "Resolver: %zu total, %zu cached, %zu need checkpoint, %zu need bloom, "
        "%zu need aggregation",
        result.all_files.size(), result.cached.size(),
        result.needs_checkpoint.size(), result.needs_bloom.size(),
        result.needs_aggregation.size());

    co_return result;
}

}  // namespace dftracer::utils::index::build
