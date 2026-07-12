#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_drain.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <set>
#include <stdexcept>
#include <system_error>

namespace dftracer::utils::utilities::composites::dft::indexing {

using aggregators::AGG_GLOBAL_CONFIG_KEY;
using aggregators::AggGlobalConfig;
using aggregators::AggregationVisitor;
using aggregators::EventAggregator;
using aggregators::serialize_agg_global_config;
using indexer::internal::get_logical_path;

coro::CoroTask<ResolverResult> resolve_and_build_index(
    CoroScope* scope, ResolveAndBuildInput input) {
    // Determine parallelism
    std::size_t parallelism = input.parallelism;
    if (parallelism == 0) {
        parallelism = dftracer_utils_hardware_concurrency();
    }

    // Initial resolve
    IndexResolverUtility resolver;
    ResolverInput resolve_input;
    resolve_input.directory = std::move(input.directory);
    resolve_input.files = std::move(input.files);
    resolve_input.index_dir = input.index_dir;
    resolve_input.require_checkpoints = input.require_checkpoints;
    resolve_input.require_bloom = input.require_bloom;
    resolve_input.require_manifest = input.require_manifest;
    resolve_input.require_aggregation = input.require_aggregation;
    resolve_input.aggregation_config = input.aggregation_config;

    auto result = co_await resolver.process(resolve_input);

    if (result.all_files.empty()) {
        co_return result;
    }

    // Neither a cached aggregation tier at a different interval nor a merged
    // aggregation polluted by a changed source can be refined in place; discard
    // every affected index root and rebuild from scratch.
    if ((result.needs_augmentation || result.stale_detected) &&
        !input.force_rebuild) {
        std::set<std::string> index_roots;
        for (const auto& file : result.all_files) {
            index_roots.insert(
                internal::determine_index_path(file, input.index_dir));
        }
        for (const auto& root : index_roots) {
            DFTRACER_UTILS_LOG_WARN("%s; rebuilding %s",
                                    result.stale_detected
                                        ? "Source changed since indexing"
                                        : "Aggregation interval changed",
                                    root.c_str());
            std::error_code ec;
            fs::remove_all(root, ec);
            if (ec) {
                throw DFTUtilsException(ErrorCode::IO,
                                        "failed to remove stale index " + root +
                                            ": " + ec.message());
            }
        }
        result = co_await resolver.process(resolve_input);
        if (result.all_files.empty()) {
            co_return result;
        }
    }

    // Collect files that need work (checkpoint or aggregation)
    // When force_rebuild is set, process all files
    std::vector<std::string> files_needing_work;
    if (input.force_rebuild) {
        // all_files is already a vector of strings
        files_needing_work = result.all_files;
    } else {
        std::set<std::string> files_needing_work_set;
        for (const auto& item : result.needs_checkpoint) {
            files_needing_work_set.insert(item.file_path);
        }
        for (const auto& item : result.needs_aggregation) {
            files_needing_work_set.insert(item.file_path);
        }
        files_needing_work.assign(files_needing_work_set.begin(),
                                  files_needing_work_set.end());
    }

    if (!files_needing_work.empty()) {
        DFTRACER_UTILS_LOG_INFO(
            "Building index for %zu files (checkpoint: %zu, aggregation: %zu)",
            files_needing_work.size(), result.needs_checkpoint.size(),
            result.needs_aggregation.size());

        // Set up aggregation components if needed
        std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> agg_db;
        std::unique_ptr<EventAggregator> merger;
        std::shared_ptr<aggregators::AggregationConfig> agg_config_ptr;

        if (input.require_aggregation && input.aggregation_config) {
            agg_db =
                EventAggregator::open_with_merge_operator(result.index_path);
            merger = std::make_unique<EventAggregator>(agg_db, 0);
            agg_config_ptr = std::make_shared<aggregators::AggregationConfig>(
                *input.aggregation_config);
        }

        auto batch_config = std::make_shared<indexer::IndexBuildBatchConfig>();
        batch_config->file_paths = std::move(files_needing_work);
        batch_config->index_dir = input.index_dir;
        batch_config->checkpoint_size = input.checkpoint_size;
        batch_config->parallelism = parallelism;
        batch_config->force_rebuild = input.force_rebuild;
        batch_config->build_manifest = input.require_manifest;
        batch_config->use_batch_write = true;
        batch_config->rebuild_root_summaries = true;

        // Attach AggregationVisitor if aggregation is required
        if (agg_db && agg_config_ptr) {
            batch_config->dft_visitor_factory =
                [agg_db, agg_config_ptr](const std::string& file_path)
                -> std::vector<
                    std::unique_ptr<composites::dft::DftEventVisitor>> {
                std::vector<std::unique_ptr<composites::dft::DftEventVisitor>>
                    visitors;
                visitors.push_back(std::make_unique<AggregationVisitor>(
                    agg_db, 0, *agg_config_ptr, file_path));
                return visitors;
            };
        }

        auto batch_result = co_await indexer::IndexBatchBuilderUtility::process(
            scope, std::move(batch_config));

        // Drain visitors and merge aggregation results
        std::vector<std::string> processed_files;
        if (merger) {
            processed_files = merge_aggregation_visitors(
                batch_result.extra_visitors, merger.get());

            // Persist accumulated min/max time bucket so a later read-only
            // reopen recovers the trace origin (otherwise time_range is
            // emitted as an absolute bucket index).
            merger->persist_time_bounds();

            // Write global config and per-file markers
            if (!processed_files.empty()) {
                namespace rcf = dftracer::utils::rocksdb::cf;
                indexer::IndexDatabase idx_db(
                    result.index_path, dftracer::utils::rocksdb::RocksDatabase::
                                           OpenMode::ReadOnly);

                auto batch = agg_db->begin_batch();

                // Write global config (0xFFFE key)
                AggGlobalConfig global_cfg;
                global_cfg.time_interval_us =
                    input.aggregation_config->time_interval_us;
                global_cfg.config_hash = 0;
                agg_db->put(batch, rcf::AGGREGATION,
                            std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
                            serialize_agg_global_config(global_cfg));

                // Write per-file markers
                for (const auto& file_path : processed_files) {
                    int file_id =
                        idx_db.get_file_info_id(get_logical_path(file_path));
                    if (file_id >= 0) {
                        agg_db->put(batch, rcf::AGGREGATION,
                                    aggregators::make_agg_file_key(file_id),
                                    std::string_view());
                    }
                }

                agg_db->commit_batch(batch);

                // Compact aggregation CFs so all Merge entries become Puts.
                // This allows concurrent ReadOnly access without merge
                // operators.
                agg_db->compact(rcf::AGGREGATION);
                agg_db->compact(rcf::SYSTEM_METRICS);
            }
        }

        // Re-resolve newly built files to get file_ids
        ResolverInput refresh_input;
        refresh_input.files.reserve(result.needs_checkpoint.size());
        for (const auto& item : result.needs_checkpoint) {
            refresh_input.files.push_back(item.file_path);
        }
        refresh_input.index_dir = input.index_dir;
        refresh_input.require_checkpoints = true;

        if (!refresh_input.files.empty()) {
            auto refresh_result = co_await resolver.process(refresh_input);

            // Merge newly indexed into cached
            for (auto& entry : refresh_result.cached) {
                result.cached.push_back(std::move(entry));
            }

            // Update needs_checkpoint with any that still failed
            result.needs_checkpoint =
                std::move(refresh_result.needs_checkpoint);
        }

        // Clear needs_aggregation since we processed them
        result.needs_aggregation.clear();
    }

    DFTRACER_UTILS_LOG_INFO(
        "Resolve complete: %zu total, %zu cached, %zu failed checkpoint",
        result.all_files.size(), result.cached.size(),
        result.needs_checkpoint.size());

    co_return result;
}

coro::CoroTask<void> ensure_indexes_fresh(CoroScope* scope,
                                          std::string directory,
                                          std::vector<std::string> files,
                                          std::string index_dir,
                                          bool force_rebuild) {
    ResolveAndBuildInput in;
    in.directory = std::move(directory);
    in.files = std::move(files);
    in.index_dir = std::move(index_dir);
    in.require_checkpoints = true;
    in.force_rebuild = force_rebuild;
    co_await resolve_and_build_index(scope, std::move(in));
}

coro::CoroTask<void> ensure_index_fresh(CoroScope* scope, std::string directory,
                                        std::string file, std::string index_dir,
                                        bool force_rebuild) {
    std::vector<std::string> files;
    if (!file.empty()) files.push_back(std::move(file));
    co_await ensure_indexes_fresh(scope, std::move(directory), std::move(files),
                                  std::move(index_dir), force_rebuild);
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
