#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/utilities/composites/composites.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_pgzip",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Parallel gzip compression for DFTracer .pfw files");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw files")
        .default_value<std::string>(".");

    program.add_argument("-v", "--verbose")
        .help("Enable verbose output")
        .flag();

    program.add_argument("--threads")
        .help("Number of threads for parallel processing")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string input_dir = program.get<std::string>("--directory");
    bool verbose = program.get<bool>("--verbose");
    std::size_t num_threads = program.get<std::size_t>("--threads");

    input_dir = fs::absolute(input_dir).string();

    // Find .pfw files
    std::vector<std::string> pfw_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".pfw") {
            pfw_files.push_back(entry.path().string());
        }
    }

    if (pfw_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw files found in directory: %s",
                                 input_dir.c_str());
        return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu .pfw files to compress",
                            pfw_files.size());
    if (verbose) {
        DFTRACER_UTILS_LOG_INFO("Using %zu threads for processing",
                                num_threads);
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // Create FileCompressor workflow
    auto compressor = std::make_shared<FileCompressor>();

    // Create batch processor for parallel compression
    auto batch_compressor =
        std::make_shared<BatchProcessorUtility<FileCompressionUtilityInput,
                                               FileCompressionUtilityOutput>>(
            [compressor](const FileCompressionUtilityInput& input,
                         TaskContext&) { return compressor->process(input); });

    // Create compression inputs
    std::vector<FileCompressionUtilityInput> compression_inputs;
    compression_inputs.reserve(pfw_files.size());
    for (const auto& file : pfw_files) {
        compression_inputs.push_back(FileCompressionUtilityInput::from_file(
            file, Z_DEFAULT_COMPRESSION));
    }

    // Execute parallel compression using pipeline with ThreadExecutor
    Pipeline pipeline("Parallel Gzip Compression");

    // Use the workflow adapter to integrate BatchProcessor into pipeline
    auto compression_task = use(batch_compressor).emit_on(pipeline);

    // Execute with thread pool
    ThreadExecutor executor(num_threads);
    executor.execute(pipeline, compression_inputs);
    std::vector<FileCompressionUtilityOutput> results = compression_task.get();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    // Report results
    std::size_t successful = 0;
    std::size_t total_original_size = 0;
    std::size_t total_compressed_size = 0;

    for (const auto& result : results) {
        if (result.success) {
            successful++;
            total_original_size += result.original_size;
            total_compressed_size += result.compressed_size;

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

    double overall_ratio = total_original_size > 0
                               ? static_cast<double>(total_compressed_size) /
                                     static_cast<double>(total_original_size) *
                                     100.0
                               : 0.0;

    printf("Gzip Completed. Processed %zu/%zu files in %.2f ms\n", successful,
           results.size(), duration.count());
    printf("Total: %zu -> %zu bytes (%.1f%% compression ratio)\n",
           total_original_size, total_compressed_size, overall_ratio);

    return successful == results.size() ? 0 : 1;
}
