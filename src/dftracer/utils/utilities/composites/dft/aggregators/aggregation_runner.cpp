#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_runner.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <utility>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {

namespace rcf = ::dftracer::utils::rocksdb::cf;
namespace idx = composites::dft::indexing;

void write_aggregation_tracking(::dftracer::utils::rocksdb::RocksDatabase* db,
                                const AggregationConfig& config,
                                const std::vector<std::string>& processed_files,
                                const std::string& index_path) {
    indexer::IndexDatabase idx_db(
        index_path,
        ::dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

    auto batch = db->begin_batch();

    AggGlobalConfig global_cfg;
    global_cfg.time_interval_us = config.time_interval_us;
    global_cfg.config_hash = 0;
    db->put(batch, rcf::AGGREGATION, std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
            serialize_agg_global_config(global_cfg));

    for (const auto& file_path : processed_files) {
        int file_id = idx_db.find_file(file_path);
        if (file_id >= 0) {
            auto key = make_agg_file_key(file_id);
            db->put(batch, rcf::AGGREGATION, key, "");
        }
    }

    db->commit_batch(batch);
}

coro::CoroTask<indexer::IndexBuildBatchResult> batch_index_and_aggregate(
    CoroScope* scope, std::vector<std::string> file_paths,
    std::string index_dir, std::size_t checkpoint_size, bool force_rebuild,
    std::size_t parallelism, AggregationConfig agg_config,
    std::shared_ptr<::dftracer::utils::rocksdb::RocksDatabase> agg_db,
    std::uint32_t config_hash) {
    auto batch_config = std::make_shared<indexer::IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(file_paths);
    batch_config->index_dir = std::move(index_dir);
    batch_config->checkpoint_size = checkpoint_size;
    batch_config->parallelism = parallelism;
    batch_config->force_rebuild = force_rebuild;
    batch_config->use_batch_write = true;

    auto agg_config_ptr =
        std::make_shared<AggregationConfig>(std::move(agg_config));
    batch_config->dft_visitor_factory =
        [agg_db, config_hash, agg_config_ptr](const std::string& file_path)
        -> std::vector<std::unique_ptr<composites::dft::DftEventVisitor>> {
        std::vector<std::unique_ptr<composites::dft::DftEventVisitor>> visitors;
        visitors.push_back(std::make_unique<AggregationVisitor>(
            agg_db, config_hash, *agg_config_ptr, file_path));
        return visitors;
    };

    co_return co_await indexer::IndexBatchBuilderUtility::process(
        scope, std::move(batch_config));
}

PerfettoTraceWriterInput build_streaming_input(
    EventAggregator* merger_ptr, const AggregationConfig* agg_config,
    const std::string* output_file, bool compress_output, int compression_level,
    PerfettoEventFormat event_format) {
    auto global_tracker = merger_ptr->build_global_tracker();

    PerfettoTraceWriterInput input;
    input.output_path = *output_file;
    input.aggregator = merger_ptr;
    input.tracker = global_tracker.get();
    input.agg_config = agg_config;
    input.owned_tracker = std::move(global_tracker);
    input.root_pids = input.tracker->get_root_pids();
    input.compute_statistics = agg_config->compute_statistics;
    input.compute_percentiles = agg_config->compute_percentiles;
    input.percentiles = agg_config->percentiles;
    input.compress = compress_output;
    input.compression_level = compression_level;
    input.format = event_format;

    const auto& intervals = input.tracker->get_all_intervals();
    if (!intervals.empty()) {
        std::uint64_t global_min = UINT64_MAX;
        std::uint64_t global_max = 0;
        for (const auto& interval : intervals) {
            global_min = std::min(global_min, interval.start_ts);
            global_max = std::max(global_max, interval.end_ts);
            auto& range = input.boundary_ranges[interval.name][interval.value];
            if (range.ts == 0 && range.te == 0) {
                range.ts = interval.start_ts;
                range.te = interval.end_ts;
            } else {
                range.ts = std::min(range.ts, interval.start_ts);
                range.te = std::max(range.te, interval.end_ts);
            }
        }
        if (global_max > global_min) {
            input.trace_duration = global_max - global_min;
        }
    }

    return input;
}

}  // namespace

coro::CoroTask<AggregationRunResult> run_aggregation(
    AggregationRunInput input) {
    AggregationRunResult result;

    if (!AggregationConfig::is_valid_format(input.output_format)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Invalid output format: %s (supported: %s)",
            input.output_format.c_str(),
            AggregationConfig::supported_formats_str().c_str());
        co_return result;
    }

    input.log_dir = fs::absolute(input.log_dir).string();
    if (input.output_file) {
        *input.output_file = fs::absolute(*input.output_file).string();
    }

    if (input.verbose) {
        std::printf("==========================================\n");
        std::printf("DFTracer Aggregator (Streaming Pipeline)\n");
        std::printf("==========================================\n");
        std::printf("Arguments:\n");
        std::printf("  Input directory: %s\n", input.log_dir.c_str());
        std::printf("  Output file: %s\n",
                    input.output_file ? input.output_file->c_str() : "<none>");
        std::printf(
            "  Time interval: %llu us\n",
            static_cast<unsigned long long>(input.agg_config.time_interval_us));
        std::printf("  Force rebuild: %s\n",
                    input.force_rebuild ? "true" : "false");
        std::printf(
            "  Checkpoint size: %zu bytes (%.2f MB)\n", input.checkpoint_size,
            static_cast<double>(input.checkpoint_size) / (1024.0 * 1024.0));
        std::printf("  Executor threads: %zu\n",
                    input.pipeline_config.executor_threads);
        std::printf("==========================================\n\n");
    }

    constexpr std::uint32_t config_hash = 0;

    ::dftracer::utils::Timer* stages = input.stages;
    ::dftracer::utils::Timer overall(true);

    auto scan_result = std::make_unique<idx::ResolverResult>();
    {
        ::dftracer::utils::ScopedTimer _t(stages, "scan_and_resolve");
        idx::IndexResolverUtility resolver;
        idx::ResolverInput resolver_input;
        resolver_input.directory = input.log_dir;
        resolver_input.index_dir = input.index_dir;
        resolver_input.require_aggregation = !input.force_rebuild;
        resolver_input.aggregation_config = input.agg_config;
        *scan_result = co_await resolver.process(resolver_input);
    }

    auto& input_files = scan_result->all_files;
    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 input.log_dir.c_str());
        co_return result;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto& shared_index_path = scan_result->index_path;
    result.index_path = shared_index_path;
    result.input_file_count = input_files.size();

    Pipeline pipeline(input.pipeline_config);

    if (input.force_rebuild && fs::exists(shared_index_path)) {
        DFTRACER_UTILS_LOG_INFO("Clearing shared index store: %s",
                                shared_index_path.c_str());
        fs::remove_all(shared_index_path);
    }

    std::shared_ptr<::dftracer::utils::rocksdb::RocksDatabase> agg_db;
    std::unique_ptr<EventAggregator> merger;
    {
        ::dftracer::utils::ScopedTimer _t(stages, "open_rocksdb");
        agg_db = EventAggregator::open_with_merge_operator(shared_index_path);
        merger = std::make_unique<EventAggregator>(agg_db, config_hash);
    }

    const std::size_t num_needing_index = scan_result->needs_checkpoint.size();
    const std::size_t num_needing_agg_only =
        input.force_rebuild ? scan_result->cached.size()
                            : scan_result->needs_aggregation.size();
    const std::size_t num_cached =
        input.force_rebuild ? 0 : scan_result->total_cached();
    result.cached_file_count = num_cached;

    std::vector<std::string> files_to_process;
    files_to_process.reserve(num_needing_index + num_needing_agg_only);
    for (auto& item : scan_result->needs_checkpoint) {
        files_to_process.push_back(std::move(item.file_path));
    }
    if (input.force_rebuild) {
        for (auto& item : scan_result->cached) {
            files_to_process.push_back(std::move(item.file_path));
        }
    } else {
        for (auto& item : scan_result->needs_aggregation) {
            files_to_process.push_back(std::move(item.file_path));
        }
    }
    result.processed_file_count = files_to_process.size();

    DFTRACER_UTILS_LOG_INFO(
        "Files to process: %zu (%zu need indexing, %zu need aggregation only, "
        "%zu cached)",
        files_to_process.size(), num_needing_index, num_needing_agg_only,
        num_cached);

    bool write_success =
        !input.output_file.has_value();  // no output -> trivially OK
    std::size_t total_keys = 0;
    std::atomic<std::size_t> perfetto_keys_written{0};

    auto main_task = make_task(
        [&](CoroScope& scope) -> coro::CoroTask<void> {
            if (!files_to_process.empty()) {
                {
                    ::dftracer::utils::ScopedTimer _t(stages,
                                                      "index_and_aggregate");
                    auto batch_result = co_await batch_index_and_aggregate(
                        &scope, files_to_process, input.index_dir,
                        input.checkpoint_size, input.force_rebuild,
                        input.pipeline_config.executor_threads,
                        input.agg_config, agg_db, config_hash);

                    {
                        ::dftracer::utils::ScopedTimer _vd(stages,
                                                           "visitor_drain");
                        for (auto& file_visitors :
                             batch_result.extra_visitors) {
                            for (auto& visitor : file_visitors) {
                                auto* agg_visitor =
                                    dynamic_cast<AggregationVisitor*>(
                                        visitor.get());
                                if (agg_visitor) {
                                    for (const auto& k :
                                         agg_visitor->observed_extra_keys())
                                        merger->add_observed_extra_key(k);
                                    for (const auto& m :
                                         agg_visitor->observed_custom_metrics())
                                        merger->add_observed_custom_metric(m);
                                    auto output = agg_visitor->take_output();
                                    merger->merge_chunk(std::move(output));
                                }
                            }
                            file_visitors.clear();
                        }
                    }
                }

                {
                    ::dftracer::utils::ScopedTimer _wt(stages,
                                                       "write_tracking");
                    write_aggregation_tracking(agg_db.get(), input.agg_config,
                                               files_to_process,
                                               shared_index_path);
                }
            }

            ::dftracer::utils::ScopedTimer _pp(stages, "post_processing");

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
            if (input.output_file &&
                input.output_format == AggregationConfig::FORMAT_ARROW) {
                using namespace ::dftracer::utils::utilities::common::arrow;

                std::unique_ptr<AssociationTracker> global_tracker;
                {
                    ::dftracer::utils::ScopedTimer _bt(stages,
                                                       "build_global_tracker");
                    global_tracker = merger->build_global_tracker();
                }
                (void)global_tracker;

                EventAggregator::ObservedColumns obs;
                {
                    ::dftracer::utils::ScopedTimer _oc(stages,
                                                       "observed_columns");
                    obs = merger->observed_columns();
                }
                auto& global_extra_key_ids = obs.extra_key_ids;
                auto& global_custom_metric_names = obs.custom_metric_names;

                IpcWriter ipc;
                if (co_await ipc.open(*input.output_file) != 0) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Failed to open Arrow IPC file: %s",
                        input.output_file->c_str());
                } else {
                    ::dftracer::utils::ScopedTimer _aw(stages,
                                                       "arrow_scan_write");
                    constexpr std::size_t BATCH_ROWS = 10000;
                    AggregationBatch batch;
                    batch.entries.reserve(BATCH_ROWS);
                    batch.global_extra_key_ids = &global_extra_key_ids;
                    batch.global_custom_metric_names =
                        &global_custom_metric_names;

                    std::vector<ArrowExportResult> pending_batches;
                    merger->scan([&](AggMapType, const AggregationKey& key,
                                     AggregationMetrics& metrics) {
                        total_keys++;
                        batch.entries.emplace_back(key, std::move(metrics));
                        if (batch.entries.size() >= BATCH_ROWS) {
                            pending_batches.push_back(batch.to_arrow());
                            batch.entries.clear();
                        }
                        return true;
                    });
                    if (!batch.entries.empty()) {
                        pending_batches.push_back(batch.to_arrow());
                    }

                    write_success = true;
                    for (auto& ab : pending_batches) {
                        if (co_await ipc.write_batch(ab) != 0) {
                            write_success = false;
                            break;
                        }
                    }
                    if (write_success) {
                        write_success = (co_await ipc.close() == 0);
                    } else {
                        co_await ipc.close();
                    }
                }
            } else
#endif
                if (input.output_file) {
                PerfettoTraceWriterInput streaming_input;
                {
                    ::dftracer::utils::ScopedTimer _si(stages,
                                                       "build_streaming_input");
                    streaming_input = build_streaming_input(
                        merger.get(), &input.agg_config, &(*input.output_file),
                        input.compress_output, input.compression_level,
                        input.event_format);
                    streaming_input.keys_written = &perfetto_keys_written;
                    streaming_input.merge_on_sharded = true;
                }
                {
                    ::dftracer::utils::ScopedTimer _pw(stages,
                                                       "perfetto_write");
                    PerfettoTraceWriterUtility writer;
                    write_success = co_await scope.spawn(
                        writer, std::move(streaming_input));
                }
                total_keys = perfetto_keys_written.load();
            } else {
                // No output file requested: just count keys in the AGGREGATION
                // CF so callers get a meaningful total_keys.
                merger->scan([&](AggMapType, const AggregationKey&,
                                 AggregationMetrics&) {
                    total_keys++;
                    return true;
                });
            }
        },
        "AggregatorMain");

    pipeline.set_source(main_task);
    {
        ::dftracer::utils::ScopedTimer _t(stages, "pipeline_execute");
        pipeline.execute();
    }

    {
        ::dftracer::utils::ScopedTimer _t(stages, "close_rocksdb");
        merger.reset();
        agg_db.reset();
    }

    overall.stop();
    result.elapsed_ms = static_cast<double>(overall.elapsed()) / 1e6;
    result.total_keys = total_keys;
    result.success = write_success;

    if (input.verbose) {
        std::printf("\n==========================================\n");
        std::printf("Aggregation Results\n");
        std::printf("==========================================\n");
        std::printf("  Execution time: %.2f seconds\n",
                    result.elapsed_ms / 1000.0);
        std::printf("  Files: %zu total, %zu processed, %zu cached\n",
                    result.input_file_count, result.processed_file_count,
                    result.cached_file_count);
        std::printf("  Unique aggregation keys: %zu\n", result.total_keys);
        if (input.output_file) {
            std::printf("  Output file: %s\n", input.output_file->c_str());
            std::printf("  Write status: %s\n",
                        result.success ? "SUCCESS" : "FAILED");
        }
        std::printf("==========================================\n");
    }

    if (stages) stages->print_stages();

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
