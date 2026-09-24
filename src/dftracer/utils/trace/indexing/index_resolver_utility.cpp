#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#include <dftracer/utils/trace/indexing/index_resolver_utility.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <unordered_map>
#include <unordered_set>

namespace dftracer::utils::trace::indexing {

namespace {

namespace rcf = dftracer::utils::rocksdb::cf;

using aggregators::AGG_FILE_KEY_LEN;
using aggregators::AGG_FILE_KEY_PREFIX;
using aggregators::AGG_GLOBAL_CONFIG_KEY;
using aggregators::deserialize_agg_global_config;
using utilities::indexer::has_capability;
using utilities::indexer::IndexDatabase;
using utilities::indexer::IndexFileEntryCapability;

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
    std::vector<std::string> bloom_fields;
    std::optional<aggregators::AggregationConfig> aggregation_config;
};

struct ResolveGroupOutput {
    std::vector<FileWorkItem> needs_checkpoint;
    std::vector<FileWorkItem> needs_bloom;
    std::vector<FileWorkItem> needs_aggregation;
    std::vector<ResolvedFile> cached;

    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us = 0;
    bool stale_detected = false;
};

bool covers(const std::vector<std::string>& indexed,
            const std::vector<std::string>& fields) {
    for (const std::string& f : fields)
        if (std::find(indexed.begin(), indexed.end(), f) == indexed.end())
            return false;
    return true;
}

ResolveGroupOutput resolve_group_sync(ResolveGroupInput input) {
    ResolveGroupOutput result;

    if (input.index_path.empty() || !fs::exists(input.index_path)) {
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), -1});
        }
        return result;
    }

    try {
        IndexDatabase db(
            input.index_path,
            dftracer::utils::utilities::indexer::IndexOpenMode::ReadOnly);
        auto registry = db.query_all_file_registry();

        bool agg_config_compatible = false;
        if (input.require_aggregation && input.aggregation_config) {
            std::string global_config_val;
            auto status =
                db.db()->get(std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
                             &global_config_val, rcf::AGGREGATION);
            if (status.ok() && !global_config_val.empty()) {
                auto global_cfg =
                    deserialize_agg_global_config(global_config_val);
                // config_hash == 0 means any config is compatible; otherwise
                // recompute with the stored time_interval before comparing.
                bool hashes_match = global_cfg.config_hash == 0;
                if (!hashes_match) {
                    auto check_config = *input.aggregation_config;
                    check_config.time_interval_us = global_cfg.time_interval_us;
                    hashes_match =
                        check_config.compute_hash() == global_cfg.config_hash;
                }
                if (hashes_match) {
                    agg_config_compatible = true;
                    result.stored_time_interval_us =
                        global_cfg.time_interval_us;
                    if (global_cfg.time_interval_us !=
                        input.aggregation_config->time_interval_us) {
                        result.needs_augmentation = true;
                    }
                }
            }
        }

        // Key existence in the AGGREGATION column family means cached.
        std::unordered_set<std::int32_t> agg_cached_file_ids;
        if (input.require_aggregation && agg_config_compatible) {
            auto iter = db.db()->new_iterator(rcf::AGGREGATION);
            if (iter) {
                iter->Seek(AGG_FILE_KEY_PREFIX);
                while (iter->Valid()) {
                    auto key = iter->key();
                    if (key.size() < AGG_FILE_KEY_LEN ||
                        key[0] != AGG_FILE_KEY_PREFIX[0] ||
                        key[1] != AGG_FILE_KEY_PREFIX[1]) {
                        break;
                    }
                    std::int32_t file_id =
                        (static_cast<std::int32_t>(
                             static_cast<std::uint8_t>(key[2]))
                         << 24) |
                        (static_cast<std::int32_t>(
                             static_cast<std::uint8_t>(key[3]))
                         << 16) |
                        (static_cast<std::int32_t>(
                             static_cast<std::uint8_t>(key[4]))
                         << 8) |
                        static_cast<std::int32_t>(
                            static_cast<std::uint8_t>(key[5]));
                    agg_cached_file_ids.insert(file_id);
                    iter->Next();
                }
            }
        }

        for (auto& f : input.files) {
            auto reg_it = registry.find(f.logical_path);
            if (reg_it == registry.end()) {
                result.needs_checkpoint.push_back(
                    FileWorkItem{f.file_index, std::move(f.file_path), -1});
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
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id});
                continue;
            }

            auto caps = reg.capabilities;
            bool has_checkpoints =
                has_capability(caps, IndexFileEntryCapability::MEMBERS) ||
                has_capability(caps, IndexFileEntryCapability::FILE_SUMMARY);
            bool has_bloom =
                has_capability(caps, IndexFileEntryCapability::BLOOM);

            if (input.require_checkpoints && !has_checkpoints) {
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id});
                continue;
            }

            // A changed checkpoint size re-checkpoints the file. The build path
            // stores the requested size verbatim, so an exact compare suffices
            // (no file read). Zero on either side means "unspecified": skip.
            if (input.require_checkpoints && input.checkpoint_size != 0) {
                const auto stored_ckpt = db.get_checkpoint_size(reg.file_id);
                if (stored_ckpt != 0 && stored_ckpt != input.checkpoint_size) {
                    result.needs_checkpoint.push_back(FileWorkItem{
                        f.file_index, std::move(f.file_path), reg.file_id});
                    continue;
                }
            }

            if (input.require_bloom &&
                (!has_bloom || !covers(db.query_index_dimensions(reg.file_id),
                                       input.bloom_fields))) {
                result.needs_bloom.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id});
                continue;
            }

            if (input.require_aggregation &&
                agg_cached_file_ids.find(reg.file_id) ==
                    agg_cached_file_ids.end()) {
                result.needs_aggregation.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id});
                continue;
            }

            result.cached.push_back(ResolvedFile{
                f.file_index, std::move(f.file_path), reg.file_id, caps});
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN(
            "Index resolve failed (%s); falling back to full rebuild",
            e.what());
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), -1});
        }
    }

    return result;
}

}  // namespace

coro::CoroTask<ResolverResult> IndexResolverUtility::operator()(
    CoroScope& ctx, const ResolverInput& input) const {
    DFTRACER_UTILS_TRACE_SCOPE("resolve index");
    ResolverResult result;

    if (!input.directory.empty()) {
        utilities::filesystem::PatternDirectoryScannerUtilityInput scan_input{
            input.directory, {".pfw", ".pfw.gz"}, false};
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
                utilities::indexer::internal::get_file_modification_time(
                    input.files[i]));
        }
    }

    if (result.all_files.empty()) {
        co_return result;
    }

    result.index_path = internal::determine_index_path(result.all_files.front(),
                                                       input.index_dir);

    std::unordered_map<std::string, std::vector<PendingFile>> groups;
    for (std::size_t i = 0; i < result.all_files.size(); ++i) {
        const auto& file_path = result.all_files[i];
        auto idx_path =
            internal::determine_index_path(file_path, input.index_dir);
        auto logical =
            utilities::indexer::internal::get_logical_path(file_path);
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
            group_input.bloom_fields = input.bloom_fields;
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
            group_input.bloom_fields = input.bloom_fields;
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
    }

    DFTRACER_UTILS_LOG_INFO(
        "Resolver: %zu total, %zu cached, %zu need checkpoint, %zu need bloom, "
        "%zu need aggregation",
        result.all_files.size(), result.cached.size(),
        result.needs_checkpoint.size(), result.needs_bloom.size(),
        result.needs_aggregation.size());

    co_return result;
}

}  // namespace dftracer::utils::trace::indexing
