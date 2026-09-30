#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/internal/utils.h>

namespace dftracer::utils::index::build {

namespace {

using index::store::IndexDatabase;

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
    bool require_aggregation;
    std::size_t checkpoint_size = 0;
    std::optional<ChunkIndexerConfig> bloom_config;
    std::string schema;
    std::optional<index::schemas::dft::agg::AggregationConfig>
        aggregation_config;
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
    bool format_outdated = false;
    std::string index_path;
};

// Whether the file's rows are in the tier, built with `params_hash`.
bool agg_current(const std::optional<index::store::ExtensionState>& state,
                 std::uint64_t params_hash) {
    return state && state->ready &&
           state->version == index::store::layout::ext_version(
                                 index::store::IndexExtension::AGG) &&
           state->params_hash == params_hash;
}

// Paths the file's pruning tier indexed by value (bloom) or range (zonemap).
std::vector<std::string> indexed_paths(const IndexDatabase& db, int file_id) {
    auto paths =
        db.extension_paths(file_id, index::store::IndexExtension::BLOOM);
    for (auto& p :
         db.extension_paths(file_id, index::store::IndexExtension::ZONEMAP))
        paths.push_back(std::move(p));
    return paths;
}

bool covers(const std::vector<std::string>& indexed,
            const std::vector<std::string>& fields) {
    for (const std::string& f : fields)
        if (std::find(indexed.begin(), indexed.end(), f) == indexed.end())
            return false;
    return true;
}

// The file's recorded schema is current and, when `wanted` is set, is it.
bool schema_current(const IndexDatabase& db, int file_id,
                    const std::string& wanted) {
    auto recorded = db.file_schema(file_id);
    if (!recorded || (!wanted.empty() && *recorded != wanted)) return false;
    const RecordSchema* schema = find_schema(*recorded);
    auto state =
        db.extension_state(file_id, index::store::IndexExtension::PROFILE);
    return schema && state && state->params_hash == schema->params_hash();
}

// The tier extensions of the file that are missing or were built with other
// settings than `base` gives its schema.
index::store::ExtensionMask stale_tier(
    const IndexDatabase& db, int file_id,
    const std::optional<ChunkIndexerConfig>& base, const RecordSchema* schema) {
    using index::store::IndexExtension;
    index::store::ExtensionMask stale;
    for (auto ext : {IndexExtension::STATS, IndexExtension::CATALOG,
                     IndexExtension::METADATA})
        if (!db.extension_current(file_id, ext)) stale.add(ext);
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
        auto state = db.extension_state(file_id, ext);
        if (!db.extension_current(file_id, ext) ||
            state->params_hash != config->params_hash(ext))
            stale.add(ext);
    }
    const bool paths = config->extensions.has(IndexExtension::BLOOM) ||
                       config->extensions.has(IndexExtension::ZONEMAP);
    if (paths &&
        !covers(indexed_paths(db, file_id), config->extra_dimensions)) {
        for (auto ext : {IndexExtension::BLOOM, IndexExtension::ZONEMAP})
            if (config->extensions.has(ext)) stale.add(ext);
    }
    for (const auto& ext : index::extensions::plugin_extensions()) {
        auto state = db.plugin_extension_state(file_id, ext->name);
        if (!state || !ext->current(*state)) {
            stale.add(IndexExtension::PLUGIN);
            break;
        }
    }
    return stale;
}

ResolveGroupOutput resolve_group_sync(ResolveGroupInput input) {
    ResolveGroupOutput result;
    result.index_path = input.index_path;
    if (!input.index_path.empty()) load_index_schemas(input.index_path);

    if (input.index_path.empty() || !fs::exists(input.index_path)) {
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), -1, {}, {}});
        }
        return result;
    }

    try {
        IndexDatabase db(input.index_path,
                         index::store::IndexOpenMode::ReadOnly);
        // An index in another format is rebuilt whole, like a changed source:
        // it is never read.
        if (db.format_outdated()) {
            DFTRACER_UTILS_LOG_WARN("Index %s is not in format %u; rebuilding",
                                    input.index_path.c_str(),
                                    IndexDatabase::FORMAT_VERSION);
            result.stale_detected = true;
            result.format_outdated = true;
            for (auto& f : input.files) {
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), -1, {}, {}});
            }
            return result;
        }
        auto registry = db.query_all_file_registry();

        // The tier holds no file id, so one file's rows cannot be replaced:
        // rows built with other params, or of a file rebuilt whole, make the
        // whole tier stale.
        const bool aggregate =
            input.require_aggregation && input.aggregation_config;
        const std::uint64_t params =
            aggregate ? input.aggregation_config->params_hash() : 0;
        if (auto stored =
                index::schemas::dft::agg::tier::read_config(*db.db())) {
            result.stored_time_interval_us = stored->time_interval_us;
            if (aggregate && stored->params_hash != params) {
                result.aggregation_stale = true;
                auto same_interval = *input.aggregation_config;
                same_interval.time_interval_us = stored->time_interval_us;
                result.needs_augmentation =
                    same_interval.params_hash() == stored->params_hash;
            }
        }
        std::vector<std::pair<FileWorkItem, bool>> agg_candidates;
        auto rebuilt_whole = [&](int file_id) {
            if (file_id >= 0 &&
                db.extension_state(file_id, index::store::IndexExtension::AGG))
                result.aggregation_stale = true;
        };

        for (auto& f : input.files) {
            auto reg_it = registry.find(f.logical_path);
            if (reg_it == registry.end()) {
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), -1, {}, {}});
                continue;
            }

            const auto& reg = reg_it->second;

            // Stat-only staleness using mtime/size captured during the scan
            // (no extra metadata op). Rebuild if the source changed since it
            // was indexed, or the record predates mtime/size tracking.
            auto stored_stat = db.get_file_stat(f.logical_path);
            bool stale = !stored_stat || stored_stat->mtime != f.mtime ||
                         stored_stat->size != f.size;
            if (stale) {
                DFTRACER_UTILS_LOG_WARN(
                    "Index stale for %s (source changed since indexing); "
                    "rebuilding",
                    f.file_path.c_str());
                result.stale_detected = true;
                rebuilt_whole(reg.file_id);
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id, {}, {}});
                continue;
            }

            const bool has_checkpoints =
                db.extension_current(reg.file_id,
                                     index::store::IndexExtension::MEMBERS) &&
                schema_current(db, reg.file_id, input.schema);

            if (input.require_checkpoints && !has_checkpoints) {
                rebuilt_whole(reg.file_id);
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id, {}, {}});
                continue;
            }

            // A changed checkpoint size re-checkpoints the file. The build path
            // stores the requested size verbatim, so an exact compare suffices
            // (no file read). Zero on either side means "unspecified": skip.
            if (input.require_checkpoints && input.checkpoint_size != 0) {
                const auto stored_ckpt = db.get_checkpoint_size(reg.file_id);
                if (stored_ckpt != 0 && stored_ckpt != input.checkpoint_size) {
                    rebuilt_whole(reg.file_id);
                    result.needs_checkpoint.push_back(
                        FileWorkItem{f.file_index,
                                     std::move(f.file_path),
                                     reg.file_id,
                                     {},
                                     {}});
                    continue;
                }
            }

            std::string schema =
                db.file_schema(reg.file_id).value_or(std::string{});
            bool needs_agg = false;
            if (aggregate) {
                if (find_schema(schema)) {
                    const auto agg_state = db.extension_state(
                        reg.file_id, index::store::IndexExtension::AGG);
                    // Rows stored by another version of the tier are of another
                    // layout and the tier cannot replace one file's rows, so
                    // the whole tier is stale: it is cleared and rebuilt, never
                    // merged onto.
                    if (agg_state && agg_state->version !=
                                         index::store::layout::ext_version(
                                             index::store::IndexExtension::AGG))
                        result.aggregation_stale = true;
                    const bool current = agg_current(agg_state, params);
                    agg_candidates.emplace_back(
                        FileWorkItem{
                            f.file_index, f.file_path, reg.file_id, {}, schema},
                        current);
                    needs_agg = !current;
                }
            }
            if (input.require_bloom) {
                auto tier = stale_tier(db, reg.file_id, input.bloom_config,
                                       find_schema(schema));
                if (!tier.empty()) {
                    result.needs_bloom.push_back(
                        FileWorkItem{f.file_index, std::move(f.file_path),
                                     reg.file_id, tier, std::move(schema)});
                    continue;
                }
            }
            if (needs_agg) continue;

            result.cached.push_back(
                ResolvedFile{f.file_index, std::move(f.file_path), reg.file_id,
                             std::move(schema)});
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
        // Every file is rebuilt whole and the tier cannot replace one file's
        // rows, so the stored tier (possibly the corrupt part) is cleared.
        result.aggregation_stale = true;
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), -1, {}, {}});
        }
    }

    return result;
}

}  // namespace

coro::CoroTask<ResolverResult> Resolver::operator()(
    CoroScope& ctx, const ResolverInput& input) const {
    DFTRACER_UTILS_TRACE_SCOPE("resolve index");
    ResolverResult result;

    if (!input.directory.empty()) {
        utilities::filesystem::PatternDirectoryScannerUtilityInput scan_input{
            input.directory, utilities::filesystem::trace_file_patterns(),
            false};
        std::vector<utilities::filesystem::FileEntry> matched;
        matched = co_await scanner_(ctx, scan_input);
        result.all_files.reserve(matched.size());
        result.all_file_sizes.reserve(matched.size());
        result.all_file_mtimes.reserve(matched.size());
        for (const auto& entry : matched) {
            result.all_files.push_back(entry.path.string());
            result.all_file_sizes.push_back(entry.size);
            result.all_file_mtimes.push_back(entry.mtime);
        }
    } else {
        result.all_files = input.files;
        result.all_file_sizes.assign(input.files.size(), 0);
        result.all_file_mtimes.assign(input.files.size(), 0);
        for (std::size_t i = 0; i < input.files.size(); ++i) {
            std::error_code ec;
            auto sz = fs::file_size(input.files[i], ec);
            if (!ec) result.all_file_sizes[i] = static_cast<std::size_t>(sz);
            result.all_file_mtimes[i] = static_cast<std::uint64_t>(
                index::store::internal::get_file_modification_time(
                    input.files[i]));
        }
    }

    if (result.all_files.empty()) {
        co_return result;
    }

    result.index_path = trace::internal::determine_index_path(
        result.all_files.front(), input.index_dir);

    StringViewMap<std::vector<PendingFile>> groups;
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
            group_input.require_aggregation = input.require_aggregation;
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
            group_input.require_aggregation = input.require_aggregation;
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
        if (out.format_outdated)
            result.outdated_roots.push_back(out.index_path);
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
