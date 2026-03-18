#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/predicate_parser_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::views;
using namespace dftracer::utils::utilities::filesystem;
using dftracer::utils::utilities::composites::dft::indexing::PredicateMap;
using dftracer::utils::utilities::composites::dft::indexing::
    PredicateParserInput;
using dftracer::utils::utilities::composites::dft::indexing::
    PredicateParserUtility;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;

// Convert parsed predicates to a ViewPredicate
static ViewPredicate predicates_to_view_predicate(const PredicateMap& preds) {
    ViewPredicate predicate;
    for (const auto& [dim, values] : preds) {
        predicate.with_bloom_dim(dim, values);
    }
    return predicate;
}

static coro::CoroTask<int> run_view(argparse::ArgumentParser& program) {
    std::string directory = program.get<std::string>("--directory");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::string preset = program.get<std::string>("--preset");
    std::string recipe_path = program.get<std::string>("--recipe");
    std::string save_recipe = program.get<std::string>("--save-recipe");
    std::string output_path = program.get<std::string>("--output");
    std::string time_range_str = program.get<std::string>("--time-range");
    double min_duration = program.get<double>("--min-duration");
    double max_duration = program.get<double>("--max-duration");
    bool stream_mode = program.get<bool>("--stream");
    bool no_metadata = program.get<bool>("--no-metadata");
    bool no_auto_index = program.get<bool>("--no-auto-index");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    auto query_strs = program.get<std::vector<std::string>>("--query");

    // Build the ViewDefinition
    ViewDefinition view;

    if (!preset.empty()) {
        if (preset == "io") {
            view = ViewDefinition::io_view();
        } else if (preset == "compute") {
            view = ViewDefinition::compute_view();
        } else if (preset == "dlio") {
            view = ViewDefinition::dlio_view();
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown preset: %s. Use: io, compute, "
                "dlio",
                preset.c_str());
            co_return 1;
        }
    } else if (!recipe_path.empty()) {
        if (!fs::exists(recipe_path)) {
            DFTRACER_UTILS_LOG_ERROR("Recipe file not found: %s",
                                     recipe_path.c_str());
            co_return 1;
        }
        std::ifstream recipe_file(recipe_path);
        std::string json_content((std::istreambuf_iterator<char>(recipe_file)),
                                 std::istreambuf_iterator<char>());
        view = ViewDefinition::from_json(json_content);
    } else {
        view.name = "custom";
        view.description = "Custom inline view";
    }

    // Parse --query into predicates using shared utility
    PredicateParserInput parser_input;
    parser_input.with_predicate_strings(query_strs);
    auto parsed = PredicateParserUtility{}.process(parser_input);
    if (parsed.success && !parsed.predicates.empty()) {
        view.with_predicate(predicates_to_view_predicate(parsed.predicates));
    }

    // Apply event-level filters to all predicates
    // Parse time range
    std::optional<std::pair<double, double>> time_range;
    if (!time_range_str.empty()) {
        auto comma = time_range_str.find(',');
        if (comma != std::string::npos) {
            double min_ts = std::stod(time_range_str.substr(0, comma));
            double max_ts = std::stod(time_range_str.substr(comma + 1));
            time_range = std::make_pair(min_ts, max_ts);
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Invalid --time-range format. Use: min,max (e.g., "
                "1000000,2000000)");
            co_return 1;
        }
    }

    // Apply event-level filters to existing predicates
    if (time_range || min_duration > 0 || max_duration > 0) {
        if (view.predicates.empty()) {
            // Create a catch-all predicate with just the event filters
            ViewPredicate pred;
            if (time_range)
                pred.with_time_range(time_range->first, time_range->second);
            if (min_duration > 0) pred.with_min_duration(min_duration);
            if (max_duration > 0) pred.with_max_duration(max_duration);
            view.with_predicate(std::move(pred));
        } else {
            for (auto& pred : view.predicates) {
                if (time_range)
                    pred.with_time_range(time_range->first, time_range->second);
                if (min_duration > 0) pred.with_min_duration(min_duration);
                if (max_duration > 0) pred.with_max_duration(max_duration);
            }
        }
    }

    // Apply metadata flag
    if (no_metadata) {
        view.with_include_metadata(false);
    }

    // Validate: must have at least one predicate
    if (view.predicates.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No view specified. Use --preset, --recipe, or --query.");
        std::cerr << program;
        co_return 1;
    }

    // Save recipe if requested
    if (!save_recipe.empty()) {
        std::ofstream out(save_recipe);
        out << view.to_json();
        out.close();
        std::printf("View recipe saved to: %s\n", save_recipe.c_str());
    }

    // Collect files
    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            co_return 1;
        }

        PatternDirectoryScannerUtility scanner;
        PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
        auto matched = co_await scanner.process(scan_input);

        for (const auto& entry : matched) {
            files.push_back(entry.path.string());
        }

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                     directory.c_str());
            co_return 1;
        }
    } else {
        files = program.get<std::vector<std::string>>("--files");

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "No files or directory specified. Use --help for usage.");
            std::cerr << program;
            co_return 1;
        }
    }

    // Auto-build indices for files missing .idx
    std::vector<std::string> files_needing_index;
    for (const auto& file_path : files) {
        std::string idx_path =
            internal::determine_index_path(file_path, index_dir);
        if (!fs::exists(idx_path)) {
            files_needing_index.push_back(file_path);
        }
    }

    if (!files_needing_index.empty()) {
        if (no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing .idx index for %zu file(s) and --no-auto-index is "
                "set. Run dftracer_index first.",
                files_needing_index.size());
            for (const auto& f : files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n", f.c_str());
            }
            co_return 1;
        }

        std::printf("Auto-building index for %zu file(s)...\n",
                    files_needing_index.size());

        auto pipeline_config = PipelineConfig()
                                   .with_name("DFTracer View Auto-Indexer")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        std::atomic<std::size_t> indexed_count{0};
        std::atomic<std::size_t> failed_count{0};

        auto index_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope(
                    [&](CoroScope& scope) -> coro::CoroTask<void> {
                        auto* indexed_count_ptr = &indexed_count;
                        auto* failed_count_ptr = &failed_count;
                        for (std::size_t i = 0; i < files_needing_index.size();
                             ++i) {
                            const auto file_path = files_needing_index[i];
                            scope.spawn([file_path, index_dir, checkpoint_size,
                                         indexed_count_ptr,
                                         failed_count_ptr](CoroScope& /*fctx*/)
                                            -> coro::CoroTask<void> {
                                IndexBuilderUtility builder;
                                auto config =
                                    IndexBuildConfig::for_file(file_path)
                                        .with_index_dir(index_dir)
                                        .with_checkpoint_size(checkpoint_size)
                                        .with_bloom(true)
                                        .with_index_threshold(0);
                                auto result = co_await builder.process(config);

                                if (result.success) {
                                    (*indexed_count_ptr)++;
                                } else {
                                    (*failed_count_ptr)++;
                                    DFTRACER_UTILS_LOG_ERROR(
                                        "Auto-indexing failed for %s: %s",
                                        file_path.c_str(),
                                        result.error_message.c_str());
                                }

                                co_return;
                            });
                        }
                        co_return;
                    });

                co_return;
            },
            "AutoIndex");

        pipeline.set_source(index_task);
        pipeline.set_destination(index_task);
        pipeline.execute();

        std::printf("Auto-indexing complete: %zu indexed, %zu failed\n",
                    indexed_count.load(), failed_count.load());
    }

    // Process files through the view pipeline
    std::mutex output_mutex;
    std::vector<std::string> all_events;
    std::atomic<std::uint64_t> total_events_matched{0};
    std::atomic<std::uint64_t> total_events_scanned{0};
    std::atomic<std::uint64_t> total_chunks_scanned{0};
    std::atomic<std::uint64_t> total_chunks_skipped{0};

    // Open output file if specified
    FILE* out_file = nullptr;
    if (!output_path.empty()) {
        out_file = std::fopen(output_path.c_str(), "w");
        if (!out_file) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open output file: %s",
                                     output_path.c_str());
            co_return 1;
        }
    }

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer View")
                               .with_compute_threads(executor_threads)
                               .with_watchdog(false);

    Pipeline pipeline(pipeline_config);

    auto view_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                auto* total_chunks_skipped_ptr = &total_chunks_skipped;
                auto* total_events_matched_ptr = &total_events_matched;
                auto* total_events_scanned_ptr = &total_events_scanned;
                auto* total_chunks_scanned_ptr = &total_chunks_scanned;
                auto* output_mutex_ptr = &output_mutex;
                auto* all_events_ptr = &all_events;
                for (std::size_t fi = 0; fi < files.size(); ++fi) {
                    const auto file_path = files[fi];
                    scope.spawn([file_path, index_dir, checkpoint_size, view,
                                 stream_mode, out_file,
                                 total_chunks_skipped_ptr,
                                 total_events_matched_ptr,
                                 total_events_scanned_ptr,
                                 total_chunks_scanned_ptr, output_mutex_ptr,
                                 all_events_ptr](
                                    CoroScope& fctx) -> coro::CoroTask<void> {
                        // Resolve paths
                        std::string idx_path = internal::determine_index_path(
                            file_path, index_dir);

                        // Collect metadata
                        auto meta_input =
                            MetadataCollectorUtilityInput::from_file(file_path)
                                .with_checkpoint_size(checkpoint_size)
                                .with_force_rebuild(false)
                                .with_index(idx_path);
                        auto metadata =
                            co_await MetadataCollectorUtility{}.process(
                                meta_input);

                        if (!metadata.success) {
                            DFTRACER_UTILS_LOG_ERROR(
                                "Failed to collect metadata for %s: %s",
                                file_path.c_str(),
                                metadata.error_message.c_str());
                            co_return;
                        }

                        // Run ViewBuilderUtility to get candidate chunks
                        ViewBuilderInput builder_input;
                        builder_input.with_view(view)
                            .with_file_path(file_path)
                            .with_idx_path(fs::exists(idx_path) ? idx_path : "")
                            .with_uncompressed_size(metadata.uncompressed_size)
                            .with_num_checkpoints(metadata.num_checkpoints);

                        ViewBuilderUtility builder;
                        auto build_output =
                            co_await builder.process(builder_input);

                        if (!build_output.success) {
                            DFTRACER_UTILS_LOG_ERROR(
                                "ViewBuilder failed for %s", file_path.c_str());
                            co_return;
                        }

                        (*total_chunks_skipped_ptr) +=
                            build_output.skipped_checkpoints;

                        if (!build_output.file_may_match) {
                            co_return;
                        }

                        // Process each candidate chunk
                        co_await fctx.scope([file_path, idx_path,
                                             checkpoint_size, view, stream_mode,
                                             out_file, output_mutex_ptr,
                                             all_events_ptr,
                                             total_events_matched_ptr,
                                             total_events_scanned_ptr,
                                             total_chunks_scanned_ptr,
                                             candidates =
                                                 build_output.candidates](
                                                CoroScope& chunk_scope)
                                                -> coro::CoroTask<void> {
                            for (const auto& candidate : candidates) {
                                chunk_scope.spawn([file_path, idx_path,
                                                   checkpoint_size, view,
                                                   stream_mode, out_file,
                                                   output_mutex_ptr,
                                                   all_events_ptr,
                                                   total_events_matched_ptr,
                                                   total_events_scanned_ptr,
                                                   total_chunks_scanned_ptr,
                                                   candidate](
                                                      CoroScope& /*cctx*/)
                                                      -> coro::CoroTask<void> {
                                    ViewReaderInput reader_input;
                                    reader_input.with_file_path(file_path)
                                        .with_idx_path(idx_path)
                                        .with_checkpoint_size(checkpoint_size)
                                        .with_byte_range(candidate.start_byte,
                                                         candidate.end_byte)
                                        .with_checkpoint_idx(
                                            candidate.checkpoint_idx)
                                        .with_view(view);

                                    ViewReaderUtility reader;
                                    auto read_output =
                                        co_await reader.process(reader_input);

                                    if (read_output.success) {
                                        (*total_events_matched_ptr) +=
                                            read_output.events_matched;
                                        (*total_events_scanned_ptr) +=
                                            read_output.events_scanned;
                                        (*total_chunks_scanned_ptr)++;

                                        if (stream_mode) {
                                            std::lock_guard<std::mutex> lock(
                                                *output_mutex_ptr);
                                            for (const auto& event :
                                                 read_output.events) {
                                                if (out_file) {
                                                    std::fprintf(out_file,
                                                                 "%s\n",
                                                                 event.c_str());
                                                } else {
                                                    std::printf("%s\n",
                                                                event.c_str());
                                                }
                                            }
                                        } else {
                                            std::lock_guard<std::mutex> lock(
                                                *output_mutex_ptr);
                                            for (auto& event :
                                                 read_output.events) {
                                                all_events_ptr->push_back(
                                                    std::move(event));
                                            }
                                        }
                                    }

                                    co_return;
                                });
                            }
                            co_return;
                        });

                        co_return;
                    });
                }
                co_return;
            });

            co_return;
        },
        "ViewProcess");

    pipeline.set_source(view_task);
    pipeline.set_destination(view_task);
    pipeline.execute();

    // Write collected output
    if (!stream_mode) {
        FILE* target = out_file ? out_file : stdout;

        for (const auto& event : all_events) {
            std::fprintf(target, "%s\n", event.c_str());
        }
    }

    if (out_file) {
        std::fclose(out_file);
    }

    // Print summary to stderr
    std::fprintf(stderr,
                 "View: %s | Files: %zu | Chunks: scanned=%llu skipped=%llu "
                 "| Events: matched=%llu scanned=%llu\n",
                 view.name.c_str(), files.size(),
                 (unsigned long long)total_chunks_scanned.load(),
                 (unsigned long long)total_chunks_skipped.load(),
                 (unsigned long long)total_events_matched.load(),
                 (unsigned long long)total_events_scanned.load());

    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_view",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Apply filtered views to DFTracer trace files. Uses bloom filter "
        "indices for efficient chunk-skipping. Supports predefined views "
        "(io, compute, dlio), custom recipes, and inline queries.");

    // Input files
    program.add_argument("--files")
        .help("Trace files to process (.pfw, .pfw.gz)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing trace files")
        .default_value<std::string>("");

    // View specification
    program.add_argument("--preset")
        .help("Predefined view: io, compute, dlio")
        .default_value<std::string>("");

    program.add_argument("--recipe")
        .help("Custom view JSON file path")
        .default_value<std::string>("");

    program.add_argument("--save-recipe")
        .help("Save the constructed view to a JSON file")
        .default_value<std::string>("");

    program.add_argument("--query")
        .help("Inline query (e.g., cat=POSIX,name=read|write)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    // Event-level filters
    program.add_argument("--time-range")
        .help(
            "Timestamp filter as min,max in microseconds (e.g., "
            "1000000,2000000)")
        .default_value<std::string>("");

    program.add_argument("--min-duration")
        .help("Minimum event duration in microseconds")
        .scan<'g', double>()
        .default_value(static_cast<double>(0.0));

    program.add_argument("--max-duration")
        .help("Maximum event duration in microseconds")
        .scan<'g', double>()
        .default_value(static_cast<double>(0.0));

    // Output
    program.add_argument("-o", "--output")
        .help("Output file path (default: stdout)")
        .default_value<std::string>("");

    program.add_argument("--stream")
        .help("Stream matching events to stdout as NDJSON")
        .flag();

    program.add_argument("--no-metadata")
        .help("Exclude metadata events (ph=M) from output")
        .flag();

    // Indexing options
    program.add_argument("--index-dir")
        .help("Directory where .idx index files are stored")
        .default_value<std::string>("");

    program.add_argument("--no-auto-index")
        .help("Disable automatic index building for files missing .idx")
        .flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for auto-indexing in bytes (default: " +
              std::to_string(
                  indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE) +
              ")")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help("Number of worker threads")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    return run_view(program).get();
}
