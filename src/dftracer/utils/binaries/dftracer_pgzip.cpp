#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/utilities/composites/composites.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::utilities::composites;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_pgzip",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Parallel gzip compression for DFTracer .pfw files using composable "
        "utilities and pipeline processing");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw files")
        .default_value<std::string>(".");

    program.add_argument("-v", "--verbose")
        .help("Enable verbose output")
        .flag();

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

    program.add_argument("-l", "--compression-level")
        .help("Compression level (0-9, default: Z_DEFAULT_COMPRESSION)")
        .scan<'d', int>()
        .default_value(Z_DEFAULT_COMPRESSION);

    program.add_argument("--disable-watchdog")
        .help("Disable watchdog for hang detection")
        .flag();

    program.add_argument("--watchdog-global-timeout")
        .help(
            "Watchdog global timeout for pipeline execution in seconds (0 = no "
            "timeout)")
        .scan<'d', int>()
        .default_value(0);

    program.add_argument("--watchdog-task-timeout")
        .help("Watchdog default task timeout in seconds (0 = no timeout)")
        .scan<'d', int>()
        .default_value(0);

    program.add_argument("--watchdog-interval")
        .help("Watchdog check interval in seconds")
        .scan<'d', int>()
        .default_value(1);

    program.add_argument("--watchdog-warning-threshold")
        .help("Watchdog long-running task warning threshold in seconds")
        .scan<'d', int>()
        .default_value(300);

    program.add_argument("--watchdog-idle-timeout")
        .help("Watchdog idle timeout in seconds (0 = use default)")
        .scan<'d', int>()
        .default_value(300);

    program.add_argument("--watchdog-deadlock-timeout")
        .help("Watchdog deadlock timeout in seconds (0 = use default)")
        .scan<'d', int>()
        .default_value(600);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program << std::endl;
        return 1;
    }

    // Parse arguments
    std::string input_dir = program.get<std::string>("--directory");
    bool verbose = program.get<bool>("--verbose");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::size_t scheduler_threads =
        program.get<std::size_t>("--scheduler-threads");
    int compression_level = program.get<int>("--compression-level");
    bool disable_watchdog = program.get<bool>("--disable-watchdog");
    int global_timeout = program.get<int>("--watchdog-global-timeout");
    int task_timeout = program.get<int>("--watchdog-task-timeout");
    int watchdog_interval = program.get<int>("--watchdog-interval");
    int warning_threshold = program.get<int>("--watchdog-warning-threshold");
    int idle_timeout = program.get<int>("--watchdog-idle-timeout");
    int deadlock_timeout = program.get<int>("--watchdog-deadlock-timeout");

    input_dir = fs::absolute(input_dir).string();

    std::printf("==========================================\n");
    std::printf("DFTracer Parallel Gzip (Pipeline Processing)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Input dir: %s\n", input_dir.c_str());
    std::printf("  Compression level: %d\n", compression_level);
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Scheduler threads: %zu\n", scheduler_threads);
    std::printf("  Verbose: %s\n", verbose ? "true" : "false");
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // ========================================================================
    // Create Pipeline with Configuration
    // ========================================================================
    auto pipeline_config =
        PipelineConfig()
            .with_name("DFTracer Parallel Gzip")
            .with_executor_threads(executor_threads)
            .with_scheduler_threads(scheduler_threads)
            .with_watchdog(!disable_watchdog)
            .with_global_timeout(std::chrono::seconds(global_timeout))
            .with_task_timeout(std::chrono::seconds(task_timeout))
            .with_watchdog_interval(std::chrono::seconds(watchdog_interval))
            .with_warning_threshold(std::chrono::seconds(warning_threshold))
            .with_executor_idle_timeout(std::chrono::seconds(idle_timeout))
            .with_executor_deadlock_timeout(
                std::chrono::seconds(deadlock_timeout));

    Pipeline pipeline(pipeline_config);

    // ========================================================================
    // Task 1: Compress Files (INTRA-TASK PARALLELISM via
    // DirectoryFileProcessorUtility)
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 1: Compressing files...");

    // Task 1.1: Input - Directory input for file discovery and compression
    auto dir_input =
        DirectoryProcessInput::from_directory(input_dir).with_extensions(
            {".pfw"});

    // Task 1.2: Output - Batch compression results
    using CompressFilesOutput =
        BatchFileProcessOutput<FileCompressionUtilityOutput>;

    // Task 1.3: Utility definition - DirectoryFileProcessorUtility with
    // FileCompressorUtility
    auto file_compressor =
        [compression_level](
            TaskContext& /*ctx*/,
            const std::string& file_path) -> FileCompressionUtilityOutput {
        auto input = FileCompressionUtilityInput::from_file(file_path,
                                                            compression_level);
        FileCompressorUtility compressor;
        return compressor.process(input);
    };

    auto compress_workflow = std::make_shared<
        DirectoryFileProcessorUtility<FileCompressionUtilityOutput>>(
        file_compressor);

    // Task 1.4: Task definition - Convert utility to task
    auto task1_compress_files = utilities::use(compress_workflow).as_task();
    task1_compress_files->with_name("CompressFiles");

    // ========================================================================
    // Task 2: Cleanup Original Files and Report Results
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s",
                            "Task 2: Setting up cleanup and reporting...");

    // Task 2.1: Input - Compression results from Task 1
    using CleanupInput = CompressFilesOutput;

    // Task 2.2: Output - Final statistics
    struct CleanupOutput {
        std::size_t successful = 0;
        std::size_t total_files = 0;
        std::size_t total_original_size = 0;
        std::size_t total_compressed_size = 0;
    };

    // Task 2.3: Utility definition - Cleanup and aggregate results
    auto cleanup_and_report_func =
        [verbose](const CleanupInput& batch_result) -> CleanupOutput {
        CleanupOutput output;
        output.total_files = batch_result.results.size();

        for (const auto& result : batch_result.results) {
            if (result.success) {
                output.successful++;
                output.total_original_size += result.original_size;
                output.total_compressed_size += result.compressed_size;

                // Remove original file after successful compression
                try {
                    fs::remove(result.input_path);
                } catch (const std::exception& e) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Failed to remove original file %s: %s",
                        result.input_path.c_str(), e.what());
                }

                if (verbose) {
                    double ratio = result.compression_ratio() * 100.0;
                    DFTRACER_UTILS_LOG_INFO(
                        "Compressed %s: %zu -> %zu bytes (%.1f%%)",
                        fs::path(result.input_path).filename().c_str(),
                        result.original_size, result.compressed_size, ratio);
                }
            } else {
                DFTRACER_UTILS_LOG_ERROR("Failed to compress %s: %s",
                                         result.input_path.c_str(),
                                         result.error_message.c_str());
            }
        }

        return output;
    };

    // Task 2.4: Task definition
    auto task2_cleanup = make_task(cleanup_and_report_func, "CleanupAndReport");

    // ========================================================================
    // Define Dependencies and Execute Pipeline
    // ========================================================================

    // Define dependencies
    task2_cleanup->depends_on(task1_compress_files);

    // Set up pipeline
    pipeline.set_source(task1_compress_files);
    pipeline.set_destination(task2_cleanup);

    // Execute pipeline with initial input
    pipeline.execute(dir_input);

    // Get final results
    auto compression_results = task1_compress_files->get<CompressFilesOutput>();
    auto cleanup_result = task2_cleanup->get<CleanupOutput>();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    // ========================================================================
    // Print Results
    // ========================================================================

    double overall_ratio =
        cleanup_result.total_original_size > 0
            ? static_cast<double>(cleanup_result.total_compressed_size) /
                  static_cast<double>(cleanup_result.total_original_size) *
                  100.0
            : 0.0;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Gzip Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Processed: %zu/%zu files\n", cleanup_result.successful,
                cleanup_result.total_files);
    std::printf("  Total: %zu -> %zu bytes (%.1f%% compression ratio)\n",
                cleanup_result.total_original_size,
                cleanup_result.total_compressed_size, overall_ratio);
    std::printf("==========================================\n");

    return cleanup_result.successful == cleanup_result.total_files ? 0 : 1;
}
