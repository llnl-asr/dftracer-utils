#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <unordered_map>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {

namespace rcf = dftracer::utils::rocksdb::cf;

using aggregators::AGG_FILE_KEY_LEN;
using aggregators::AGG_FILE_KEY_PREFIX;
using aggregators::AGG_GLOBAL_CONFIG_KEY;
using aggregators::deserialize_agg_global_config;
using indexer::has_capability;
using indexer::IndexDatabase;
using indexer::IndexFileEntryCapability;

struct PendingFile {
    std::size_t file_index;
    std::string file_path;
    std::string logical_path;
};

struct ResolveGroupInput {
    std::string index_path;
    std::vector<PendingFile> files;
    bool require_checkpoints;
    bool require_bloom;
    bool require_manifest;
    bool require_aggregation;
    std::optional<aggregators::AggregationConfig> aggregation_config;
};

struct ResolveGroupOutput {
    std::vector<FileWorkItem> needs_checkpoint;
    std::vector<FileWorkItem> needs_bloom;
    std::vector<FileWorkItem> needs_manifest;
    std::vector<FileWorkItem> needs_aggregation;
    std::vector<ResolvedFile> cached;
    bool success = true;
    std::string error_message;

    // Aggregation augmentation info
    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us = 0;
};

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
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        auto registry = db.query_all_file_registry();

        // Check global aggregation config first
        bool agg_config_compatible = false;
        if (input.require_aggregation && input.aggregation_config) {
            std::string global_config_val;
            auto status =
                db.db()->get(std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
                             &global_config_val, rcf::AGGREGATION);
            if (status.ok() && !global_config_val.empty()) {
                auto global_cfg =
                    deserialize_agg_global_config(global_config_val);
                // config_hash == 0 means "any config is compatible"
                // Otherwise recompute hash with stored time_interval to check
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

        // Build set of file_ids with aggregation data (key existence = cached)
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
            auto caps = reg.capabilities;
            bool has_checkpoints =
                has_capability(caps, IndexFileEntryCapability::CHECKPOINTS) ||
                has_capability(caps, IndexFileEntryCapability::FILE_SUMMARY);
            bool has_bloom =
                has_capability(caps, IndexFileEntryCapability::BLOOM);
            bool has_manifest =
                has_capability(caps, IndexFileEntryCapability::MANIFEST);

            if (input.require_checkpoints && !has_checkpoints) {
                result.needs_checkpoint.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id});
                continue;
            }

            if (input.require_bloom && !has_bloom) {
                result.needs_bloom.push_back(FileWorkItem{
                    f.file_index, std::move(f.file_path), reg.file_id});
                continue;
            }

            if (input.require_manifest && !has_manifest) {
                result.needs_manifest.push_back(FileWorkItem{
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

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        for (auto& f : input.files) {
            result.needs_checkpoint.push_back(
                FileWorkItem{f.file_index, std::move(f.file_path), -1});
        }
    }

    return result;
}

}  // namespace

coro::CoroTask<ResolverResult> IndexResolverUtility::process(
    const ResolverInput& input) {
    ResolverResult result;

    if (!input.directory.empty()) {
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            input.directory, {".pfw", ".pfw.gz"}, false};
        std::vector<filesystem::FileEntry> matched;
        if (this->has_context()) {
            matched = co_await this->context().spawn(scanner_, scan_input);
        } else {
            matched = co_await scanner_.process(scan_input);
        }
        result.all_files.reserve(matched.size());
        result.all_file_sizes.reserve(matched.size());
        for (const auto& entry : matched) {
            result.all_files.push_back(entry.path.string());
            result.all_file_sizes.push_back(entry.size);
        }
    } else {
        result.all_files = input.files;
        result.all_file_sizes.assign(input.files.size(), 0);
        for (std::size_t i = 0; i < input.files.size(); ++i) {
            std::error_code ec;
            auto sz = fs::file_size(input.files[i], ec);
            if (!ec) result.all_file_sizes[i] = static_cast<std::size_t>(sz);
        }
    }

    if (result.all_files.empty()) {
        co_return result;
    }

    result.index_path = internal::determine_index_path(result.all_files.front(),
                                                       input.index_dir);

    // Group files by index path and prepare for resolution
    std::unordered_map<std::string, std::vector<PendingFile>> groups;
    for (std::size_t i = 0; i < result.all_files.size(); ++i) {
        const auto& file_path = result.all_files[i];
        auto idx_path =
            internal::determine_index_path(file_path, input.index_dir);
        auto logical = indexer::internal::get_logical_path(file_path);
        groups[idx_path].push_back(
            PendingFile{i, file_path, std::move(logical)});
    }

    std::vector<ResolveGroupOutput> outputs;
    outputs.reserve(groups.size());

    if (this->has_context() && groups.size() > 1) {
        std::vector<coro::SpawnFuture<ResolveGroupOutput>> futures;
        futures.reserve(groups.size());

        for (auto& [idx_path, files] : groups) {
            ResolveGroupInput group_input;
            group_input.index_path = idx_path;
            group_input.files = std::move(files);
            group_input.require_checkpoints = input.require_checkpoints;
            group_input.require_bloom = input.require_bloom;
            group_input.require_manifest = input.require_manifest;
            group_input.require_aggregation = input.require_aggregation;
            group_input.aggregation_config = input.aggregation_config;

            futures.push_back(this->context().spawn(
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
            group_input.require_manifest = input.require_manifest;
            group_input.require_aggregation = input.require_aggregation;
            group_input.aggregation_config = input.aggregation_config;

            outputs.push_back(resolve_group_sync(std::move(group_input)));
        }
    }

    // Merge results
    for (auto& out : outputs) {
        for (auto& item : out.needs_checkpoint) {
            result.needs_checkpoint.push_back(std::move(item));
        }
        for (auto& item : out.needs_bloom) {
            result.needs_bloom.push_back(std::move(item));
        }
        for (auto& item : out.needs_manifest) {
            result.needs_manifest.push_back(std::move(item));
        }
        for (auto& item : out.needs_aggregation) {
            result.needs_aggregation.push_back(std::move(item));
        }
        for (auto& item : out.cached) {
            result.cached.push_back(std::move(item));
        }
        // Merge augmentation info (all groups should have same global config)
        if (out.needs_augmentation) {
            result.needs_augmentation = true;
        }
        if (out.stored_time_interval_us != 0) {
            result.stored_time_interval_us = out.stored_time_interval_us;
        }
    }

    DFTRACER_UTILS_LOG_INFO(
        "Resolver: %zu total, %zu cached, %zu need checkpoint, %zu need bloom, "
        "%zu need manifest, %zu need aggregation",
        result.all_files.size(), result.cached.size(),
        result.needs_checkpoint.size(), result.needs_bloom.size(),
        result.needs_manifest.size(), result.needs_aggregation.size());

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
