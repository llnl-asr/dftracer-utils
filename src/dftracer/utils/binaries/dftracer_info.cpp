#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/utilities/composites/composites.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace dftracer::utils;

struct FileInfo {
    std::string file_path;
    std::string idx_path;
    bool has_index;
    bool index_valid;
    std::uint64_t compressed_size;
    std::uint64_t uncompressed_size;
    std::uint64_t num_lines;
    std::uint64_t checkpoint_size;
    std::size_t num_checkpoints;
    ArchiveFormat format;
    bool success;
    std::string error_msg;
};

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

static FileInfo get_file_info(const std::string& file_path,
                              const std::string& index_dir,
                              std::size_t checkpoint_size, bool force_rebuild) {
    FileInfo info;
    info.file_path = file_path;
    info.has_index = false;
    info.index_valid = false;
    info.compressed_size = 0;
    info.uncompressed_size = 0;
    info.num_lines = 0;
    info.checkpoint_size = 0;
    info.num_checkpoints = 0;
    info.success = false;

    try {
        if (!fs::exists(file_path)) {
            info.error_msg = "File does not exist";
            return info;
        }

        info.format = IndexerFactory::detect_format(file_path);
        info.compressed_size = fs::file_size(file_path);

        info.idx_path = utilities::composites::dft::determine_index_path(
            file_path, index_dir);

        info.has_index = fs::exists(info.idx_path);

        std::shared_ptr<Indexer> indexer;
        if (!info.has_index || force_rebuild) {
            if (force_rebuild && info.has_index) {
                DFTRACER_UTILS_LOG_DEBUG("Removing existing index: %s",
                                         info.idx_path.c_str());
                fs::remove(info.idx_path);
            }
            DFTRACER_UTILS_LOG_DEBUG("Building index for: %s",
                                     file_path.c_str());
            indexer = IndexerFactory::create(file_path, info.idx_path,
                                             checkpoint_size, true);
            indexer->build();
            info.has_index = true;
        } else {
            indexer = IndexerFactory::create(file_path, info.idx_path,
                                             checkpoint_size, false);
            if (indexer->need_rebuild()) {
                DFTRACER_UTILS_LOG_DEBUG("Index needs rebuild: %s",
                                         info.idx_path.c_str());
                info.index_valid = false;
                fs::remove(info.idx_path);
                indexer = IndexerFactory::create(file_path, info.idx_path,
                                                 checkpoint_size, true);
                indexer->build();
            }
        }

        info.index_valid = true;
        info.uncompressed_size = indexer->get_max_bytes();
        info.num_lines = indexer->get_num_lines();
        info.checkpoint_size = indexer->get_checkpoint_size();
        info.num_checkpoints = indexer->get_checkpoints().size();

        info.success = true;

    } catch (const std::exception& e) {
        info.error_msg = e.what();
        info.success = false;
    }

    return info;
}

static void print_file_info(const FileInfo& info, bool verbose) {
    std::printf("File: %s\n", info.file_path.c_str());

    if (!info.success) {
        std::printf("  Status: ERROR - %s\n", info.error_msg.c_str());
        return;
    }

    std::printf("  Format: %s\n", get_format_name(info.format));
    std::printf("  Compressed Size: %s (%llu bytes)\n",
                format_size(info.compressed_size).c_str(),
                (unsigned long long)info.compressed_size);
    std::printf("  Uncompressed Size: %s (%llu bytes)\n",
                format_size(info.uncompressed_size).c_str(),
                (unsigned long long)info.uncompressed_size);

    if (info.compressed_size > 0) {
        double ratio =
            100.0 * (1.0 - static_cast<double>(info.compressed_size) /
                               static_cast<double>(info.uncompressed_size));
        std::printf("  Compression Ratio: %.2f%%\n", ratio);
    }

    std::printf("  Number of Lines: %llu\n",
                (unsigned long long)info.num_lines);

    if (verbose) {
        std::printf("  Index File: %s\n", info.idx_path.c_str());
        std::printf("  Index Exists: %s\n", info.has_index ? "Yes" : "No");
        std::printf("  Index Valid: %s\n", info.index_valid ? "Yes" : "No");
        std::printf("  Checkpoint Size: %s (%llu bytes)\n",
                    format_size(info.checkpoint_size).c_str(),
                    (unsigned long long)info.checkpoint_size);
        std::printf("  Number of Checkpoints: %zu\n", info.num_checkpoints);

        if (info.num_checkpoints > 0 && info.uncompressed_size > 0) {
            std::uint64_t avg_chunk =
                info.uncompressed_size / info.num_checkpoints;
            std::printf("  Average Chunk Size: %s\n",
                        format_size(avg_chunk).c_str());
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
        "Display metadata and index information for DFTracer compressed files");

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

    program.add_argument("--threads")
        .help(
            "Number of threads for parallel processing (default: number of CPU "
            "cores)")
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

    std::vector<std::string> files;
    std::string directory = program.get<std::string>("--directory");
    bool verbose = program.get<bool>("--verbose");
    bool force_rebuild = program.get<bool>("--force-rebuild");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t num_threads = program.get<std::size_t>("--threads");

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

    auto start_time = std::chrono::high_resolution_clock::now();

    std::printf("===========================================\n");
    std::printf("DFTracer File Information\n");
    std::printf("===========================================\n\n");

    Pipeline pipeline;
    auto task_result =
        pipeline.add_task<std::vector<std::string>, std::vector<FileInfo>>(
            [checkpoint_size, force_rebuild, index_dir](
                std::vector<std::string> file_list,
                TaskContext& p_ctx) -> std::vector<FileInfo> {
                std::vector<TaskResult<FileInfo>::Future> futures;
                futures.reserve(file_list.size());

                for (const auto& file_path : file_list) {
                    auto task = p_ctx.emit<std::string, FileInfo>(
                        [checkpoint_size, force_rebuild, &index_dir](
                            std::string path, TaskContext&) -> FileInfo {
                            return get_file_info(path, index_dir,
                                                 checkpoint_size,
                                                 force_rebuild);
                        },
                        Input{file_path});
                    futures.push_back(std::move(task.future()));
                }

                std::vector<FileInfo> results;
                results.reserve(file_list.size());
                for (auto& future : futures) {
                    results.push_back(future.get());
                }
                return results;
            });

    ThreadExecutor executor(num_threads);
    executor.execute(pipeline, files);
    std::vector<FileInfo> all_info = task_result.get();

    std::uint64_t total_compressed = 0;
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_lines = 0;
    std::size_t successful = 0;

    for (const auto& info : all_info) {
        print_file_info(info, verbose);

        if (info.success) {
            successful++;
            total_compressed += info.compressed_size;
            total_uncompressed += info.uncompressed_size;
            total_lines += info.num_lines;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (files.size() > 1) {
        std::printf("===========================================\n");
        std::printf("Summary\n");
        std::printf("===========================================\n");
        std::printf("Total Files: %zu\n", files.size());
        std::printf("Successful: %zu\n", successful);
        std::printf("Failed: %zu\n", files.size() - successful);
        std::printf("Total Lines: %llu\n", (unsigned long long)total_lines);
        std::printf("Total Compressed: %s\n",
                    format_size(total_compressed).c_str());
        std::printf("Total Uncompressed: %s\n",
                    format_size(total_uncompressed).c_str());

        if (total_uncompressed > 0) {
            double ratio =
                100.0 * (1.0 - static_cast<double>(total_compressed) /
                                   static_cast<double>(total_uncompressed));
            std::printf("Overall Compression: %.2f%%\n", ratio);
        }

        std::printf("Processing Time: %.2f seconds\n",
                    duration.count() / 1000.0);
    }

    return (successful == files.size()) ? 0 : 1;
}
