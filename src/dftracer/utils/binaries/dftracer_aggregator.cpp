#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

static coro::CoroTask<int> run_aggregator(argparse::ArgumentParser& program) {
    std::string log_dir = program.get<std::string>("--directory");
    std::string output_file = program.get<std::string>("--output");
    double time_interval_seconds = program.get<double>("--time-interval");
    std::uint64_t time_interval_us =
        static_cast<std::uint64_t>(time_interval_seconds * 1000000.0);
    std::string group_keys_str = program.get<std::string>("--group-keys");
    std::string metric_fields_str = program.get<std::string>("--metric-fields");
    std::string categories_str = program.get<std::string>("--categories");
    std::string names_str = program.get<std::string>("--names");
    bool force_rebuild = program.get<bool>("--force");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::string index_dir = program.get<std::string>("--index-dir");
    bool compress_output = program.get<bool>("--compress");
    int compression_level = program.get<int>("--compression-level");
    std::string boundary_events_str =
        program.get<std::string>("--boundary-events");
    bool no_track_parents = program.get<bool>("--no-track-process-parents");
    std::size_t chunk_size_mb = program.get<std::size_t>("--chunk-size");
    std::size_t batch_size_mb = program.get<std::size_t>("--read-batch-size");
    std::string event_format_str = program.get<std::string>("--event-format");
    bool compute_percentiles = program.get<bool>("--compute-percentiles");
    std::string percentiles_str = program.get<std::string>("--percentiles");
    double relative_accuracy = program.get<double>("--relative-accuracy");

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

    if (compress_output) {
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
    std::vector<std::string> include_categories = split_string(categories_str);
    std::vector<std::string> include_names = split_string(names_str);

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

    std::string temp_index_dir;
    if (index_dir.empty()) {
        try {
            auto temp_path = fs::temp_directory_path();
            temp_path /= "dftracer_idx_" + std::to_string(std::time(nullptr)) +
                         "_" + std::to_string(getpid());
            temp_index_dir = temp_path.string();
            fs::create_directories(temp_index_dir);
            index_dir = temp_index_dir;
            DFTRACER_UTILS_LOG_INFO("Created temporary index directory: %s",
                                    index_dir.c_str());
        } catch (const std::filesystem::filesystem_error& e) {
            temp_index_dir = "/tmp/dftracer_idx_" +
                             std::to_string(std::time(nullptr)) + "_" +
                             std::to_string(getpid());
            fs::create_directories(temp_index_dir);
            index_dir = temp_index_dir;
            DFTRACER_UTILS_LOG_WARN(
                "Failed to get system temp directory, using /tmp: %s",
                e.what());
            DFTRACER_UTILS_LOG_INFO("Created temporary index directory: %s",
                                    index_dir.c_str());
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
    std::printf("  Time interval: %.2f seconds (%llu us)\n",
                time_interval_seconds,
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
    agg_config.include_categories = include_categories;
    agg_config.include_names = include_names;
    agg_config.compute_statistics = true;
    agg_config.compute_percentiles = compute_percentiles;
    agg_config.sketch_accuracy = relative_accuracy;
    agg_config.percentiles = percentiles;
    agg_config.boundary_events = boundary_events;
    agg_config.track_process_parents = !no_track_parents;

    // Discover input files
    filesystem::PatternDirectoryScannerUtility scanner;
    filesystem::PatternDirectoryScannerUtilityInput scan_input{
        log_dir, {".pfw", ".pfw.gz"}, false};
    auto matched_entries = co_await scanner.process(scan_input);

    std::vector<std::string> input_files;
    input_files.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.push_back(entry.path.string());
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 log_dir.c_str());
        co_return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer Aggregator")
                               .with_compute_threads(executor_threads)
                               .with_watchdog(false);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    EventAggregatorUtility merger;
    std::atomic<int> global_chunk_idx{0};

    // Streaming aggregation: file producers -> chunk workers -> merger
    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto chunk_chan = coro::make_channel<ChunkAggregatorInput>(0);
            auto result_chan = coro::make_channel<ChunkAggregationOutput>(8);

            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                // Pre-register all file producers to prevent
                // premature channel closure
                chunk_chan->register_producers(input_files.size());

                // File producers: one per input file
                for (const auto& file_path : input_files) {
                    auto* global_chunk_idx_ptr = &global_chunk_idx;
                    scope.spawn([file_path, chunk_chan, index_dir,
                                 checkpoint_size, force_rebuild, agg_config,
                                 chunk_size_mb, batch_size_mb,
                                 global_chunk_idx_ptr](CoroScope& /*fctx*/)
                                    -> coro::CoroTask<void> {
                        [[maybe_unused]] auto producer_guard =
                            chunk_chan->adopt_producer();
                        // Build index
                        std::string idx_path =
                            composites::dft::internal::determine_index_path(
                                file_path, index_dir);
                        auto idx_input =
                            composites::dft::IndexBuildUtilityInput::from_file(
                                file_path)
                                .with_checkpoint_size(checkpoint_size)
                                .with_force_rebuild(force_rebuild)
                                .with_index(idx_path);
                        composites::dft::IndexBuilderUtility{}.process(
                            idx_input);

                        // Collect metadata
                        auto meta_input =
                            composites::dft::MetadataCollectorUtilityInput::
                                from_file(file_path)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_force_rebuild(force_rebuild)
                                    .with_index(idx_path);
                        auto metadata =
                            co_await composites::dft::MetadataCollectorUtility{}
                                .process(meta_input);

                        if (!metadata.success) {
                            DFTRACER_UTILS_LOG_WARN("Skipping file: %s",
                                                    file_path.c_str());
                            co_return;
                        }

                        // Create chunks for this file
                        FileChunkMapperUtility file_mapper;
                        auto file_chunks = co_await file_mapper.process(
                            FileChunkMapperInput::from_metadata(metadata)
                                .with_config(agg_config)
                                .with_checkpoint_size(checkpoint_size)
                                .with_target_chunk_size(chunk_size_mb)
                                .with_batch_size(batch_size_mb * 1024 * 1024));

                        int start_idx = global_chunk_idx_ptr->fetch_add(
                            static_cast<int>(file_chunks.size()));
                        for (int i = 0;
                             i < static_cast<int>(file_chunks.size()); ++i) {
                            file_chunks[i].chunk_index = start_idx + i;
                        }

                        for (auto& chunk : file_chunks) {
                            if (!co_await chunk_chan->send(std::move(chunk))) {
                                co_return;
                            }
                        }

                        co_return;
                    });
                }

                // Pre-register all chunk workers as producers
                // on result_chan
                result_chan->register_producers(executor_threads);

                // Chunk workers: parallel aggregation
                for (std::size_t w = 0; w < executor_threads; ++w) {
                    (void)w;
                    scope.spawn([chunk_chan, result_chan](
                                    CoroScope& wctx) -> coro::CoroTask<void> {
                        [[maybe_unused]] auto producer_guard =
                            result_chan->adopt_producer();
                        while (auto input = co_await wctx.receive(chunk_chan)) {
                            ChunkAggregatorUtility agg;
                            auto output = co_await agg.process(*input);
                            if (!co_await result_chan->send(
                                    std::move(output))) {
                                co_return;
                            }
                        }
                        co_return;
                    });
                }

                // Streaming merger: incremental merge
                auto* merger_ptr = &merger;
                scope.spawn([result_chan, merger_ptr](
                                CoroScope& mctx) -> coro::CoroTask<void> {
                    while (auto output = co_await mctx.receive(result_chan)) {
                        merger_ptr->merge_chunk(std::move(*output));
                    }
                    co_return;
                });

                co_return;
            });

            co_return;
        },
        "StreamingAggregate");

    // Post-processing: finalize, resolve associations, write output
    bool write_success = false;
    EventAggregatorUtilityOutput agg_results;

    auto post_task = make_task(
        [&](CoroScope& /*ctx*/) -> coro::CoroTask<bool> {
            agg_results = merger.finalize();

            // Resolve associations
            AssociationResolverInput resolver_input;
            resolver_input.aggregations = agg_results;
            resolver_input.trackers = agg_results.trackers;
            resolver_input.config = agg_config;

            AssociationResolverUtility resolver;
            auto resolver_output = co_await resolver.process(resolver_input);
            agg_results = resolver_output.aggregations;

            // Write output
            DFTRACER_UTILS_LOG_INFO("Writing %zu aggregation keys to %s%s...",
                                    agg_results.aggregations.size(),
                                    output_file.c_str(),
                                    compress_output ? " (compressed)" : "");

            if (agg_results.aggregations.empty()) {
                DFTRACER_UTILS_LOG_WARN("No aggregations to write!");
                co_return false;
            }

            PerfettoTraceWriterUtility writer;
            PerfettoTraceWriterInput writer_input{
                output_file,
                resolver_output,
                agg_config.compute_statistics,
                agg_config.compute_percentiles,
                agg_config.percentiles,
                compress_output,
                compression_level,
                event_format};
            bool success = co_await writer.process(writer_input);

            if (success) {
                DFTRACER_UTILS_LOG_INFO("Output written successfully to: %s",
                                        output_file.c_str());
                if (fs::exists(output_file)) {
                    auto file_size = fs::file_size(output_file);
                    DFTRACER_UTILS_LOG_INFO("File exists, size: %zu bytes",
                                            file_size);
                } else {
                    DFTRACER_UTILS_LOG_ERROR(
                        "File does not exist after write!");
                    success = false;
                }
            } else {
                DFTRACER_UTILS_LOG_ERROR("Failed to write output file");
            }

            write_success = success;
            co_return success;
        },
        "PostProcess");

    post_task->depends_on(streaming_task);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(post_task);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Aggregation Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Files processed: %zu\n", agg_results.total_files_processed);
    std::printf("  Bytes processed: %.2f MB\n",
                static_cast<double>(agg_results.total_bytes_processed) /
                    (1024.0 * 1024.0));
    std::printf("  Events processed: %zu\n",
                agg_results.total_events_processed);
    std::printf("  Unique aggregation keys: %zu\n",
                agg_results.aggregations.size());
    std::printf("  Throughput: %.2f MB/s, %.2f events/s\n",
                (static_cast<double>(agg_results.total_bytes_processed) /
                 (1024.0 * 1024.0)) /
                    (duration.count() / 1000.0),
                static_cast<double>(agg_results.total_events_processed) /
                    (duration.count() / 1000.0));
    std::printf("  Output file: %s\n", output_file.c_str());
    std::printf("  Write status: %s\n", write_success ? "SUCCESS" : "FAILED");
    std::printf("==========================================\n");

    AggregatorSummaryUtility summary_writer;
    summary_writer.process(agg_results);

    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary index directory: %s",
                                temp_index_dir.c_str());
        fs::remove_all(temp_index_dir);
    }

    co_return agg_results.success&& write_success ? 0 : 1;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE) +
        " B (" +
        std::to_string(indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE /
                       (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_aggregator",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Aggregate DFTracer events into time-series counters using streaming "
        "coroutine pipeline with minimal memory footprint");

    program.add_argument("-d", "--directory")
        .help("Input directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-o", "--output")
        .help("Output file path for aggregated counters")
        .default_value<std::string>("aggregated_output.json");

    program.add_argument("-t", "--time-interval")
        .help("Time interval in seconds for bucketing (default: 5.0)")
        .scan<'g', double>()
        .default_value(5.0);

    program.add_argument("-g", "--group-keys")
        .help(
            "Comma-separated extra group keys from args (e.g., "
            "epoch,step,level)")
        .default_value<std::string>("");

    program.add_argument("-m", "--metric-fields")
        .help(
            "Comma-separated custom metric fields from args (e.g., "
            "iter_count,num_events)")
        .default_value<std::string>("");

    program.add_argument("-c", "--categories")
        .help("Include only these categories (comma-separated, empty = all)")
        .default_value<std::string>("");

    program.add_argument("-n", "--names")
        .help("Include only these event names (comma-separated, empty = all)")
        .default_value<std::string>("");

    program.add_argument("-f", "--force").help("Force index recreation").flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number of CPU cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    program.add_argument("--compress")
        .help("Compress output using gzip")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--compression-level")
        .help("Gzip compression level (0-9, default: 6)")
        .scan<'d', int>()
        .default_value(6);

    program.add_argument("--boundary-events")
        .help(
            "Boundary event configuration: event_name:value_field:output_name "
            "(e.g., \"epoch.block:iter_count:epoch\")")
        .default_value<std::string>("");

    program.add_argument("--no-track-process-parents")
        .help(
            "Disable tracking of process parent relationships from fork/spawn")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--chunk-size")
        .help("Target chunk size in MB for parallel processing (default: 4)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(4));

    program.add_argument("--read-batch-size")
        .help(
            "Batch read size in MB for stream processing (default: 4, higher = "
            "faster but more memory)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(4));

    program.add_argument("--event-format")
        .help(
            "Perfetto event format: 'counter' (ph=C, point-in-time, default), "
            "'async' (ph=b/e, async tracks with overlaps), "
            "'regular' (ph=X, duration events with original TID)")
        .default_value<std::string>("counter");

    program.add_argument("--compute-percentiles")
        .help(
            "Enable percentile/quantile computation using DDSketch (opt-in due "
            "to memory overhead)")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--percentiles")
        .help(
            "Comma-separated percentiles to compute (e.g., "
            "\"0.25,0.5,0.75,0.90\" for P25, P50, P75, P90)")
        .default_value<std::string>("0.25,0.5,0.75,0.90");

    program.add_argument("--relative-accuracy")
        .help(
            "Relative accuracy for DDSketch percentile estimation "
            "(default: 0.01 = 1%)")
        .scan<'g', double>()
        .default_value(0.01);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::fprintf(stderr, "%s\n", program.help().str().c_str());
        return 1;
    }

    return run_aggregator(program).get();
}
