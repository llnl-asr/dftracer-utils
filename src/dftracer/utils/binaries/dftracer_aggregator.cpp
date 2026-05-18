#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include "common_cli.h"
#include "dftracer/utils/core/utils/timer.h"
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#endif
#include <sstream>
#include <unordered_set>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

class AggregatorArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{
        cli::DirMode::DEFAULT_DOT,
        "Input directory containing .pfw or .pfw.gz files"};
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::QueryArgs query_args{
        "Query DSL filter (e.g., 'cat == \"POSIX\" and dur > 1000')"};

    std::string output;
    double time_interval = 5000.0;
    std::string group_keys;
    std::string metric_fields;
    bool compress = false;
    int compression_level = 1;
    std::string boundary_events;
    bool no_track_parents = false;
    std::size_t chunk_size = 4;
    std::size_t read_batch_size = 4;
    std::string event_format = "counter";
    bool compute_percentiles = false;
    std::string percentiles = "0.25,0.5,0.75,0.90";
    double relative_accuracy = 0.01;
    std::string format = "json";
    bool no_default_args = false;

    explicit AggregatorArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store index files (default: system temp directory)";
        indexing.force_help = "Force index recreation";
        schema(directory, pipeline, indexing, query_args);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-o", "--output")
            .help("Output file path for aggregated counters")
            .default_value<std::string>("aggregated_output.json");

        parser()
            .add_argument("-t", "--time-interval")
            .help("Time interval in milliseconds for bucketing (default: 5000)")
            .scan<'g', double>()
            .default_value(5000.0);

        parser()
            .add_argument("-g", "--group-keys")
            .help(
                "Comma-separated extra group keys from args (e.g., "
                "epoch,step,level)")
            .default_value<std::string>("");

        parser()
            .add_argument("-m", "--metric-fields")
            .help(
                "Comma-separated custom metric fields from args (e.g., "
                "iter_count,num_events)")
            .default_value<std::string>("");

        parser()
            .add_argument("--compress")
            .help("Compress output using gzip")
            .default_value(false)
            .implicit_value(true);

        parser()
            .add_argument("--compression-level")
            .help("Gzip compression level (0-9, default: 1)")
            .scan<'d', int>()
            .default_value(1);

        parser()
            .add_argument("--boundary-events")
            .help(
                "Boundary event configuration: "
                "event_name:value_field:output_name "
                "(e.g., \"epoch.block:iter_count:epoch\")")
            .default_value<std::string>("");

        parser()
            .add_argument("--no-track-process-parents")
            .help(
                "Disable tracking of process parent relationships from "
                "fork/spawn")
            .default_value(false)
            .implicit_value(true);

        parser()
            .add_argument("--chunk-size")
            .help(
                "Target chunk size in MB for parallel processing (default: 4)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(4));

        parser()
            .add_argument("--read-batch-size")
            .help(
                "Batch read size in MB for stream processing (default: 4, "
                "higher = "
                "faster but more memory)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(4));

        parser()
            .add_argument("--event-format")
            .help(
                "Perfetto event format: 'counter' (ph=C, point-in-time, "
                "default), "
                "'async' (ph=b/e, async tracks with overlaps), "
                "'regular' (ph=X, duration events with original TID)")
            .default_value<std::string>("counter");

        parser()
            .add_argument("--compute-percentiles")
            .help(
                "Enable percentile/quantile computation using DDSketch (opt-in "
                "due "
                "to memory overhead)")
            .default_value(false)
            .implicit_value(true);

        parser()
            .add_argument("--percentiles")
            .help(
                "Comma-separated percentiles to compute (e.g., "
                "\"0.25,0.5,0.75,0.90\" for P25, P50, P75, P90)")
            .default_value<std::string>("0.25,0.5,0.75,0.90");

        parser()
            .add_argument("--relative-accuracy")
            .help(
                "Relative accuracy for DDSketch percentile estimation "
                "(default: 0.01 = 1%)")
            .scan<'g', double>()
            .default_value(0.01);

        parser()
            .add_argument("--format")
            .help(
                "Output format: 'json' (Perfetto JSON, default) or "
                "'arrow' (Arrow IPC file, .arrows extension)")
            .default_value<std::string>("json");

        parser()
            .add_argument("--no-default-args")
            .help(
                "Disable automatic aggregation of numeric event args "
                "(offset, whence, flags, etc.)")
            .default_value(false)
            .implicit_value(true);
    }

    void post_parse() override {
        output = parser().get<std::string>("--output");
        time_interval = parser().get<double>("--time-interval");
        group_keys = parser().get<std::string>("--group-keys");
        metric_fields = parser().get<std::string>("--metric-fields");
        compress = parser().get<bool>("--compress");
        compression_level = parser().get<int>("--compression-level");
        boundary_events = parser().get<std::string>("--boundary-events");
        no_track_parents = parser().get<bool>("--no-track-process-parents");
        chunk_size = parser().get<std::size_t>("--chunk-size");
        read_batch_size = parser().get<std::size_t>("--read-batch-size");
        event_format = parser().get<std::string>("--event-format");
        compute_percentiles = parser().get<bool>("--compute-percentiles");
        percentiles = parser().get<std::string>("--percentiles");
        relative_accuracy = parser().get<double>("--relative-accuracy");
        format = parser().get<std::string>("--format");
        no_default_args = parser().get<bool>("--no-default-args");
    }
};

// Write global config and per-file tracking entries.
static void write_aggregation_tracking(
    dftracer::utils::rocksdb::RocksDatabase* db,
    const AggregationConfig& config,
    const std::vector<std::string>& processed_files,
    const std::string& index_path) {
    namespace rcf = dftracer::utils::rocksdb::cf;

    // Open index database to get file_ids
    indexer::IndexDatabase idx_db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

    auto batch = db->begin_batch();

    // Write global config once
    AggGlobalConfig global_cfg;
    global_cfg.time_interval_us = config.time_interval_us;
    global_cfg.config_hash = 0;
    db->put(batch, rcf::AGGREGATION, std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
            serialize_agg_global_config(global_cfg));

    // Per-file: empty value (presence = aggregated)
    for (const auto& file_path : processed_files) {
        int file_id = idx_db.find_file(file_path);
        if (file_id >= 0) {
            auto key = make_agg_file_key(file_id);
            db->put(batch, rcf::AGGREGATION, key, "");
        }
    }

    db->commit_batch(batch);
}

static coro::CoroTask<indexer::IndexBuildBatchResult> batch_index_and_aggregate(
    CoroScope* scope, std::vector<std::string> file_paths,
    std::string index_dir, std::size_t checkpoint_size, bool force_rebuild,
    std::size_t parallelism, AggregationConfig agg_config,
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> agg_db,
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

static PerfettoTraceWriterInput build_streaming_input(
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

static coro::CoroTask<int> run_aggregator(const AggregatorArgParse* cli) {
    auto log_dir = cli->directory.value;
    auto output_file = cli->output;
    auto time_interval_ms = cli->time_interval;
    std::uint64_t time_interval_us =
        static_cast<std::uint64_t>(time_interval_ms * 1000.0);
    const auto& group_keys_str = cli->group_keys;
    const auto& metric_fields_str = cli->metric_fields;
    const auto& query_str = cli->query_args.query;
    auto force_rebuild = cli->indexing.force;
    auto checkpoint_size = cli->indexing.checkpoint_size;
    auto executor_threads = cli->pipeline.executor_threads;
    auto index_dir = cli->indexing.index_dir;
    auto compress_output = cli->compress;
    auto compression_level = cli->compression_level;
    const auto& boundary_events_str = cli->boundary_events;
    auto no_track_parents = cli->no_track_parents;
    const auto& event_format_str = cli->event_format;
    auto compute_percentiles = cli->compute_percentiles;
    const auto& percentiles_str = cli->percentiles;
    auto relative_accuracy = cli->relative_accuracy;
    const auto& output_format = cli->format;

    if (!AggregationConfig::is_valid_format(output_format)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Invalid output format: %s (supported: %s)", output_format.c_str(),
            AggregationConfig::supported_formats_str().c_str());
        co_return 1;
    }

    PerfettoEventFormat event_format = PerfettoEventFormat::COUNTER;
    if (event_format_str == "async") {
        event_format = PerfettoEventFormat::ASYNC;
    } else if (event_format_str == "regular") {
        event_format = PerfettoEventFormat::REGULAR;
    } else if (event_format_str != "counter") {
        DFTRACER_UTILS_LOG_ERROR(
            "Invalid event format: %s (must be 'counter', 'async', or "
            "'regular')",
            event_format_str.c_str());
        co_return 1;
    }

    if (output_format == AggregationConfig::FORMAT_ARROW) {
        constexpr std::string_view ext = ".arrows";
        if (output_file.size() < ext.size() ||
            output_file.substr(output_file.size() - ext.size()) != ext) {
            output_file += ext;
        }
    } else if (compress_output) {
        if (output_file.size() < 3 ||
            output_file.substr(output_file.size() - 3) != ".gz") {
            output_file += ".gz";
        }
    }

    auto split_string = [](const std::string& str) {
        std::vector<std::string> result;
        if (str.empty()) return result;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                result.push_back(item);
            }
        }
        return result;
    };

    std::vector<std::string> group_keys = split_string(group_keys_str);
    std::vector<std::string> metric_fields = split_string(metric_fields_str);
    std::vector<double> percentiles;
    if (compute_percentiles) {
        auto percentile_strs = split_string(percentiles_str);
        for (const auto& p_str : percentile_strs) {
            try {
                double p = std::stod(p_str);
                if (p >= 0.0 && p <= 1.0) {
                    percentiles.push_back(p);
                } else {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Invalid percentile value: %s (must be in [0.0, 1.0])",
                        p_str.c_str());
                    co_return 1;
                }
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Failed to parse percentile: %s",
                                         p_str.c_str());
                co_return 1;
            }
        }
        if (percentiles.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "No valid percentiles specified with --compute-percentiles");
            co_return 1;
        }
    }

    log_dir = fs::absolute(log_dir).string();
    output_file = fs::absolute(output_file).string();

    std::printf("==========================================\n");
    std::printf("DFTracer Aggregator (Streaming Pipeline)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Input directory: %s\n", log_dir.c_str());
    std::printf("  Output file: %s\n", output_file.c_str());
    std::printf("  Time interval: %.2f ms (%llu us)\n", time_interval_ms,
                static_cast<unsigned long long>(time_interval_us));
    std::printf("  Force rebuild: %s\n", force_rebuild ? "true" : "false");
    std::printf("  Checkpoint size: %zu bytes (%.2f MB)\n", checkpoint_size,
                static_cast<double>(checkpoint_size) / (1024.0 * 1024.0));
    std::printf("  Executor threads: %zu\n", executor_threads);

    if (!group_keys.empty()) {
        std::printf("  Extra group keys: ");
        for (std::size_t i = 0; i < group_keys.size(); ++i) {
            std::printf("%s%s", group_keys[i].c_str(),
                        i < group_keys.size() - 1 ? ", " : "\n");
        }
    }

    if (!metric_fields.empty()) {
        std::printf("  Custom metric fields: ");
        for (std::size_t i = 0; i < metric_fields.size(); ++i) {
            std::printf("%s%s", metric_fields[i].c_str(),
                        i < metric_fields.size() - 1 ? ", " : "\n");
        }
    }

    std::printf("==========================================\n\n");

    std::vector<BoundaryEventConfig> boundary_events;
    if (!boundary_events_str.empty()) {
        std::stringstream ss(boundary_events_str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            std::stringstream item_ss(item);
            std::string event_name, value_field, output_name;

            if (std::getline(item_ss, event_name, ':') &&
                std::getline(item_ss, value_field, ':') &&
                std::getline(item_ss, output_name, ':')) {
                BoundaryEventConfig config;
                config.event_name = event_name;
                config.value_field = value_field;
                config.output_name = output_name;
                boundary_events.push_back(config);
            }
        }
    }

    AggregationConfig agg_config;
    agg_config.time_interval_us = time_interval_us;
    agg_config.extra_group_keys = group_keys;
    agg_config.custom_metric_fields = metric_fields;
    agg_config.compute_statistics = true;
    agg_config.compute_percentiles = compute_percentiles;
    agg_config.sketch_accuracy = relative_accuracy;
    agg_config.percentiles = percentiles;
    agg_config.boundary_events = boundary_events;
    agg_config.track_process_parents = !no_track_parents;
    agg_config.track_default_args = !cli->no_default_args;

    if (!query_str.empty()) {
        DFTRACER_UTILS_LOG_WARN(
            "--query is not yet supported in fused mode, ignoring");
    }

    // Use hash=0 for simplicity (no config-based filtering)
    constexpr std::uint32_t config_hash = 0;

    Timer stages_storage("dftracer_aggregator");
    Timer* stages = cli->pipeline.time_profiling ? &stages_storage : nullptr;
    Timer overall(true);

    namespace idx = composites::dft::indexing;

    auto scan_result = std::make_unique<idx::ResolverResult>();
    {
        ScopedTimer _t(stages, "scan_and_resolve");
        idx::IndexResolverUtility resolver;
        idx::ResolverInput input;
        input.directory = log_dir;
        input.index_dir = index_dir;
        input.require_aggregation = !force_rebuild;
        input.aggregation_config = agg_config;
        *scan_result = co_await resolver.process(input);
    }

    auto& input_files = scan_result->all_files;
    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 log_dir.c_str());
        co_return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto& shared_index_path = scan_result->index_path;

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Aggregator", cli->pipeline);

    Pipeline pipeline(pipeline_config);

    if (force_rebuild && fs::exists(shared_index_path)) {
        DFTRACER_UTILS_LOG_INFO("Clearing shared index store: %s",
                                shared_index_path.c_str());
        fs::remove_all(shared_index_path);
    }

    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> agg_db;
    std::unique_ptr<EventAggregator> merger;
    {
        ScopedTimer _t(stages, "open_rocksdb");
        agg_db = EventAggregator::open_with_merge_operator(shared_index_path);
        merger = std::make_unique<EventAggregator>(agg_db, config_hash);
    }

    // Files to process: needs_checkpoint (index + aggregate) +
    // needs_aggregation
    const std::size_t num_needing_index = scan_result->needs_checkpoint.size();
    const std::size_t num_needing_agg_only =
        force_rebuild ? scan_result->cached.size()
                      : scan_result->needs_aggregation.size();
    const std::size_t num_cached =
        force_rebuild ? 0 : scan_result->total_cached();

    std::vector<std::string> files_to_process;
    files_to_process.reserve(num_needing_index + num_needing_agg_only);
    for (auto& item : scan_result->needs_checkpoint) {
        files_to_process.push_back(std::move(item.file_path));
    }
    if (force_rebuild) {
        for (auto& item : scan_result->cached) {
            files_to_process.push_back(std::move(item.file_path));
        }
    } else {
        for (auto& item : scan_result->needs_aggregation) {
            files_to_process.push_back(std::move(item.file_path));
        }
    }

    DFTRACER_UTILS_LOG_INFO(
        "Files to process: %zu (%zu need indexing, %zu need aggregation only, "
        "%zu cached)",
        files_to_process.size(), num_needing_index, num_needing_agg_only,
        num_cached);

    bool write_success = false;
    std::size_t total_keys = 0;
    std::atomic<std::size_t> perfetto_keys_written{0};

    auto main_task = make_task(
        [&](CoroScope& scope) -> coro::CoroTask<void> {
            if (!files_to_process.empty()) {
                {
                    ScopedTimer _t(stages, "index_and_aggregate");
                    auto batch_result = co_await batch_index_and_aggregate(
                        &scope, files_to_process, index_dir, checkpoint_size,
                        force_rebuild, executor_threads, agg_config, agg_db,
                        config_hash);

                    {
                        ScopedTimer _vd(stages, "visitor_drain");
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

                // Write tracking entries for processed files
                {
                    ScopedTimer _wt(stages, "write_tracking");
                    write_aggregation_tracking(agg_db.get(), agg_config,
                                               files_to_process,
                                               shared_index_path);
                }
            }

            ScopedTimer _pp(stages, "post_processing");

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
            if (output_format == AggregationConfig::FORMAT_ARROW) {
                using namespace utilities::common::arrow;

                std::unique_ptr<AssociationTracker> global_tracker;
                {
                    ScopedTimer _bt(stages, "build_global_tracker");
                    global_tracker = merger->build_global_tracker();
                }
                (void)global_tracker;

                EventAggregator::ObservedColumns obs;
                {
                    ScopedTimer _oc(stages, "observed_columns");
                    obs = merger->observed_columns();
                }
                auto& global_extra_key_ids = obs.extra_key_ids;
                auto& global_custom_metric_names = obs.custom_metric_names;

                IpcWriter ipc;
                if (co_await ipc.open(output_file) != 0) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Failed to open Arrow IPC file: %s",
                        output_file.c_str());
                } else {
                    ScopedTimer _aw(stages, "arrow_scan_write");
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
            {
                PerfettoTraceWriterInput streaming_input;
                {
                    ScopedTimer _si(stages, "build_streaming_input");
                    streaming_input = build_streaming_input(
                        merger.get(), &agg_config, &output_file,
                        compress_output, compression_level, event_format);
                    streaming_input.keys_written = &perfetto_keys_written;
                    streaming_input.merge_on_sharded = true;
                }
                {
                    ScopedTimer _pw(stages, "perfetto_write");
                    PerfettoTraceWriterUtility writer;
                    write_success = co_await scope.spawn(
                        writer, std::move(streaming_input));
                }
                total_keys = perfetto_keys_written.load();
            }
        },
        "AggregatorMain");

    pipeline.set_source(main_task);
    {
        ScopedTimer _t(stages, "pipeline_execute");
        pipeline.execute();
    }

    {
        ScopedTimer _t(stages, "close_rocksdb");
        merger.reset();
        agg_db.reset();
    }

    overall.stop();
    double duration_ms = static_cast<double>(overall.elapsed()) / 1e6;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Aggregation Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration_ms / 1000.0);
    std::printf("  Files: %zu total, %zu processed, %zu cached\n",
                input_files.size(), files_to_process.size(), num_cached);
    std::printf("  Unique aggregation keys: %zu\n", total_keys);
    std::printf("  Output file: %s\n", output_file.c_str());
    std::printf("  Write status: %s\n", write_success ? "SUCCESS" : "FAILED");
    std::printf("==========================================\n");

    if (stages) stages->print_stages();

    co_return write_success ? 0 : 1;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_aggregator",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Aggregate DFTracer events into time-series counters using streaming "
        "coroutine pipeline with minimal memory footprint");

    AggregatorArgParse cli(program);
    cli.setup();
    if (!cli.parse(argc, argv)) return 1;

    return run_aggregator(&cli).get();
}
