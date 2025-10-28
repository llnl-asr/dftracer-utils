#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <dftracer/utils/utilities/composites/dft/dft.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;

// ============================================================================
// Helper Functions
// ============================================================================

static std::string format_size(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " "
        << units[unit_index];
    return oss.str();
}

static void print_file_info(const MetadataCollectorUtilityOutput& info,
                            bool verbose) {
    std::printf("========================================\n");
    std::printf("File: %s\n", info.file_path.c_str());
    std::printf("========================================\n");

    if (!info.success) {
        std::printf("  Status: ERROR - %s\n", info.error_message.c_str());
        std::printf("\n");
        return;
    }

    // Basic Information
    std::printf("Basic Information:\n");
    std::printf("  Format: %s\n", get_format_name(info.format));
    std::printf("  Status: %s\n", "OK");

    // File Size Information
    std::printf("\nFile Size:\n");
    std::printf("  Compressed:   %12s (%llu bytes)\n",
                format_size(info.compressed_size).c_str(),
                (unsigned long long)info.compressed_size);
    std::printf("  Uncompressed: %12s (%llu bytes)\n",
                format_size(info.uncompressed_size).c_str(),
                (unsigned long long)info.uncompressed_size);

    if (info.compressed_size > 0 && info.uncompressed_size > 0 &&
        info.compressed_size != info.uncompressed_size) {
        double ratio =
            100.0 * (1.0 - static_cast<double>(info.compressed_size) /
                               static_cast<double>(info.uncompressed_size));
        double compression_factor =
            static_cast<double>(info.uncompressed_size) /
            static_cast<double>(info.compressed_size);
        std::printf(
            "  Savings:      %12s (%.2f%% reduction)\n",
            format_size(info.uncompressed_size - info.compressed_size).c_str(),
            ratio);
        std::printf("  Ratio:        %.2fx compression\n", compression_factor);
    }

    // Content Information
    std::printf("\nContent:\n");
    std::printf("  Total Lines: %llu\n", (unsigned long long)info.num_lines);
    std::printf("  Valid Events: %zu (estimated)\n", info.valid_events);

    if (info.num_lines > 0) {
        std::printf("  Avg Bytes/Line: %.2f bytes\n",
                    static_cast<double>(info.uncompressed_size) /
                        static_cast<double>(info.num_lines));
    }

    if (info.valid_events > 0) {
        std::printf("  Avg Bytes/Event: %.2f bytes\n",
                    static_cast<double>(info.uncompressed_size) /
                        static_cast<double>(info.valid_events));
    }

    // Index Information (always show if index-capable format)
    if (info.format == ArchiveFormat::GZIP ||
        info.format == ArchiveFormat::TAR_GZ) {
        std::printf("\nIndex Information:\n");
        std::printf("  Index File: %s\n", info.idx_path.empty()
                                              ? "(auto-generated)"
                                              : info.idx_path.c_str());
        std::printf("  Index Status: %s\n",
                    info.has_index ? (info.index_valid ? "Valid" : "Invalid")
                                   : "Not Created");

        if (info.has_index && info.index_valid) {
            std::printf("  Checkpoint Size: %s (%llu bytes)\n",
                        format_size(info.checkpoint_size).c_str(),
                        (unsigned long long)info.checkpoint_size);
            std::printf("  Number of Checkpoints: %zu\n", info.num_checkpoints);

            if (info.num_checkpoints > 0) {
                std::uint64_t avg_chunk =
                    info.uncompressed_size / info.num_checkpoints;
                std::uint64_t lines_per_checkpoint =
                    info.num_lines / info.num_checkpoints;
                std::printf("  Avg Chunk Size: %s (%llu bytes)\n",
                            format_size(avg_chunk).c_str(),
                            (unsigned long long)avg_chunk);
                std::printf("  Avg Lines/Checkpoint: %llu\n",
                            (unsigned long long)lines_per_checkpoint);

                // Calculate index overhead
                if (fs::exists(info.idx_path)) {
                    std::uint64_t index_size = fs::file_size(info.idx_path);
                    double index_overhead =
                        100.0 * static_cast<double>(index_size) /
                        static_cast<double>(info.compressed_size);
                    std::printf("  Index File Size: %s (%llu bytes)\n",
                                format_size(index_size).c_str(),
                                (unsigned long long)index_size);
                    std::printf("  Index Overhead: %.2f%% of compressed size\n",
                                index_overhead);
                }
            }
        }
    }

    // Detailed Statistics (verbose mode)
    if (verbose) {
        std::printf("\nDetailed Statistics:\n");
        std::printf("  Start Line: %zu\n", info.start_line);
        std::printf("  End Line: %zu\n", info.end_line);
        std::printf("  Size (MB): %.6f\n", info.size_mb);
        std::printf("  MB per Event: %.8f\n", info.size_per_line);

        // Performance estimates
        if (info.num_checkpoints > 0 && info.num_lines > 0) {
            std::uint64_t lines_per_checkpoint =
                info.num_lines / info.num_checkpoints;
            std::printf("\nRandom Access Performance:\n");
            std::printf("  Worst-case lines to scan: %llu (1 checkpoint)\n",
                        (unsigned long long)lines_per_checkpoint);
            std::printf(
                "  Best-case lines to scan: 1 (exact checkpoint hit)\n");
            std::printf("  Avg lines to scan: %llu (0.5 checkpoint)\n",
                        (unsigned long long)(lines_per_checkpoint / 2));
        }

        // Memory estimates
        if (info.checkpoint_size > 0) {
            std::printf("\nMemory Estimates:\n");
            std::printf("  Memory for 1 checkpoint: ~%s\n",
                        format_size(info.checkpoint_size).c_str());
            if (info.num_checkpoints > 0) {
                std::uint64_t total_memory_for_all =
                    info.checkpoint_size * info.num_checkpoints;
                std::printf("  Memory for all checkpoints: ~%s\n",
                            format_size(total_memory_for_all).c_str());
            }
        }
    }

    std::printf("\n");
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_info",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Display metadata and index information for DFTracer compressed files "
        "using composable utilities and pipeline processing");

    program.add_argument("--files")
        .help("Compressed files to inspect (GZIP, TAR.GZ)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing files to inspect")
        .default_value<std::string>("");

    program.add_argument("-v", "--verbose")
        .help("Show detailed information including index details")
        .flag();

    program.add_argument("-f", "--force-rebuild")
        .help("Force rebuild index files")
        .flag();

    program.add_argument("-c", "--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number of CPU cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--scheduler-threads")
        .help("Number of scheduler threads (default: 1, typically not changed)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(1));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program;
        return 1;
    }

    // Parse arguments
    std::string directory = program.get<std::string>("--directory");
    bool verbose = program.get<bool>("--verbose");
    bool force_rebuild = program.get<bool>("--force-rebuild");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::size_t scheduler_threads =
        program.get<std::size_t>("--scheduler-threads");

    // Collect files to process
    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            return 1;
        }

        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string path = entry.path().string();
                std::string ext = entry.path().extension().string();
                if (ext == ".gz") {
                    files.push_back(path);
                }
            }
        }

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "No compressed files found in directory: %s",
                directory.c_str());
            return 1;
        }
    } else {
        files = program.get<std::vector<std::string>>("--files");

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "No files or directory specified. Use --help for usage.");
            std::cerr << program;
            return 1;
        }
    }

    std::printf("==========================================\n");
    std::printf("DFTracer File Information (Pipeline Processing)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Files to process: %zu\n", files.size());
    std::printf("  Checkpoint size: %zu bytes\n", checkpoint_size);
    std::printf("  Force rebuild: %s\n", force_rebuild ? "true" : "false");
    std::printf("  Index dir: %s\n",
                index_dir.empty() ? "(auto)" : index_dir.c_str());
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Scheduler threads: %zu\n", scheduler_threads);
    std::printf("  Verbose: %s\n", verbose ? "true" : "false");
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // ========================================================================
    // Create Pipeline with Configuration
    // ========================================================================
    auto pipeline_config = PipelineConfigManager()
                               .with_name("DFTracer File Info")
                               .with_executor_threads(executor_threads)
                               .with_scheduler_threads(scheduler_threads);

    Pipeline pipeline(pipeline_config);

    // ========================================================================
    // Task 1: Collect Metadata (INTRA-TASK PARALLELISM via
    // BatchProcessorUtility)
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 1: Collecting metadata...");

    // Task 1.1: Input - List of file paths
    using MetadataInputList = std::vector<std::string>;

    // Task 1.2: Output - Batch metadata results
    using MetadataOutputList = std::vector<MetadataCollectorUtilityOutput>;

    // Task 1.3: Utility definition - Create batch processor for metadata
    // collection
    auto metadata_collector = std::make_shared<MetadataCollectorUtility>();
    auto batch_processor =
        std::make_shared<BatchProcessorUtility<MetadataCollectorUtilityInput,
                                               MetadataCollectorUtilityOutput>>(
            metadata_collector);

    // Task 1.4: Function to create inputs from file paths
    auto create_inputs_func = [checkpoint_size, force_rebuild,
                               index_dir](const MetadataInputList& file_paths)
        -> std::vector<MetadataCollectorUtilityInput> {
        std::vector<MetadataCollectorUtilityInput> inputs;
        inputs.reserve(file_paths.size());

        for (const auto& file_path : file_paths) {
            auto input = MetadataCollectorUtilityInput::from_file(file_path)
                             .with_checkpoint_size(checkpoint_size)
                             .with_force_rebuild(force_rebuild);

            if (!index_dir.empty()) {
                input.with_index(determine_index_path(file_path, index_dir));
            }

            inputs.push_back(input);
        }

        DFTRACER_UTILS_LOG_INFO("Created %zu metadata collection inputs",
                                inputs.size());
        return inputs;
    };

    // Task 1.5: Create transformation task
    auto task1_create_inputs =
        make_task(create_inputs_func, "CreateMetadataInputs");

    // Task 1.6: Convert batch processor to task
    auto task1_collect_metadata = utilities::use(batch_processor).as_task();
    task1_collect_metadata->with_name("CollectMetadata");

    // Task 1.7: Link tasks
    task1_collect_metadata->depends_on(task1_create_inputs);

    // ========================================================================
    // Task 2: Aggregate and Print Results
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 2: Setting up result aggregation...");

    // Task 2.1: Input - Metadata results from Task 1
    using AggregationInput = MetadataOutputList;

    // Task 2.2: Output - Aggregated statistics
    struct AggregationOutput {
        std::uint64_t total_compressed = 0;
        std::uint64_t total_uncompressed = 0;
        std::uint64_t total_lines = 0;
        std::size_t successful = 0;
        std::size_t total_files = 0;
    };

    // Task 2.3: Utility definition - Print and aggregate results
    auto aggregate_results_func =
        [verbose](const AggregationInput& all_info) -> AggregationOutput {
        AggregationOutput output;
        output.total_files = all_info.size();

        for (const auto& info : all_info) {
            print_file_info(info, verbose);

            if (info.success) {
                output.successful++;
                output.total_compressed += info.compressed_size;
                output.total_uncompressed += info.uncompressed_size;
                output.total_lines += info.num_lines;
            }
        }

        return output;
    };

    // Task 2.4: Task definition
    auto task2_aggregate =
        make_task(aggregate_results_func, "AggregateResults");

    // ========================================================================
    // Define Dependencies and Execute Pipeline
    // ========================================================================

    // Define dependencies
    task2_aggregate->depends_on(task1_collect_metadata);

    // Set up pipeline
    pipeline.set_source(task1_create_inputs);
    pipeline.set_destination(task2_aggregate);

    // Execute pipeline with initial input
    pipeline.execute(files);

    // Get final results
    auto metadata_results = task1_collect_metadata->get<MetadataOutputList>();
    auto aggregation_result = task2_aggregate->get<AggregationOutput>();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    // ========================================================================
    // Print Summary
    // ========================================================================

    if (files.size() > 1) {
        std::printf("==========================================\n");
        std::printf("Summary\n");
        std::printf("==========================================\n");
        std::printf("Total Files: %zu\n", aggregation_result.total_files);
        std::printf("Successful: %zu\n", aggregation_result.successful);
        std::printf("Failed: %zu\n", aggregation_result.total_files -
                                         aggregation_result.successful);
        std::printf("Total Lines: %llu\n",
                    (unsigned long long)aggregation_result.total_lines);
        std::printf("Total Compressed: %s\n",
                    format_size(aggregation_result.total_compressed).c_str());
        std::printf("Total Uncompressed: %s\n",
                    format_size(aggregation_result.total_uncompressed).c_str());

        if (aggregation_result.total_uncompressed > 0) {
            double ratio =
                100.0 * (1.0 - static_cast<double>(
                                   aggregation_result.total_compressed) /
                                   static_cast<double>(
                                       aggregation_result.total_uncompressed));
            std::printf("Overall Compression: %.2f%%\n", ratio);
        }

        std::printf("Processing Time: %.2f seconds\n",
                    duration.count() / 1000.0);
    }

    return (aggregation_result.successful == aggregation_result.total_files)
               ? 0
               : 1;
}
