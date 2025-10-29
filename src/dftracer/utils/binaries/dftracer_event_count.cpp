#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/line_processor.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/utilities/composites/composites.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <vector>

using namespace dftracer::utils;

std::size_t count_events_in_file(const std::string& gz_path,
                                 const std::string& idx_path,
                                 std::size_t checkpoint_size);

std::size_t count_events_in_pfw_file(const std::string& pfw_path);

static std::size_t process_files_parallel(const std::vector<std::string>& files,
                                          std::size_t checkpoint_size,
                                          bool force_rebuild,
                                          const std::string& index_dir,
                                          TaskContext& ctx) {
    std::vector<TaskResult<std::size_t>::Future> futures;
    futures.reserve(files.size());

    auto process_file = [checkpoint_size, force_rebuild, &index_dir](
                            std::string file_path,
                            TaskContext&) -> std::size_t {
        // Check if it's a .pfw.gz file or plain .pfw file
        static constexpr std::string_view pfw_gz_suffix = ".pfw.gz";
        static constexpr std::string_view pfw_suffix = ".pfw";

        if (file_path.size() >= pfw_gz_suffix.size() &&
            file_path.compare(file_path.size() - pfw_gz_suffix.size(),
                              pfw_gz_suffix.size(), pfw_gz_suffix) == 0) {
            std::string idx_path =
                utilities::composites::dft::determine_index_path(file_path,
                                                                 index_dir);

            if (force_rebuild && fs::exists(idx_path)) {
                fs::remove(idx_path);
            }

            try {
                return count_events_in_file(file_path, idx_path,
                                            checkpoint_size);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Failed to process file %s: %s",
                                         file_path.c_str(), e.what());
                return 0;
            }
        } else if (file_path.size() >= pfw_suffix.size() &&
                   file_path.compare(file_path.size() - pfw_suffix.size(),
                                     pfw_suffix.size(), pfw_suffix) == 0) {
            // Handle plain .pfw files with direct reading
            try {
                return count_events_in_pfw_file(file_path);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Failed to process file %s: %s",
                                         file_path.c_str(), e.what());
                return 0;
            }
        } else {
            DFTRACER_UTILS_LOG_ERROR("Unknown file type: %s",
                                     file_path.c_str());
            return 0;
        }
    };

    for (const auto& file_path : files) {
        auto task_result =
            ctx.emit<std::string, std::size_t>(process_file, Input{file_path});
        futures.push_back(std::move(task_result.future()));
    }

    std::size_t total_count = 0;
    for (auto& future : futures) {
        total_count += future.get();
    }

    return total_count;
}

class LineCounter : public LineProcessor {
   private:
    std::atomic<std::size_t> count_{0};

   public:
    LineCounter() = default;

    bool process(const char* data, std::size_t length) override {
        const char* trimmed;
        std::size_t trimmed_length;
        if (json_trim_and_validate(data, length, trimmed, trimmed_length) &&
            trimmed_length > 8) {
            count_++;
        }
        return true;
    }

    std::size_t get_count() const { return count_.load(); }
    void reset() { count_ = 0; }
};

std::size_t count_events_in_file(const std::string& gz_path,
                                 const std::string& idx_path,
                                 std::size_t checkpoint_size) {
    try {
        if (!fs::exists(idx_path)) {
            DFTRACER_UTILS_LOG_DEBUG("Building index for %s", gz_path.c_str());
            auto indexer = IndexerFactory::create(gz_path, idx_path,
                                                  checkpoint_size, true);
            indexer->build();
        } else {
            auto indexer = IndexerFactory::create(gz_path, idx_path,
                                                  checkpoint_size, false);
            if (indexer->need_rebuild()) {
                DFTRACER_UTILS_LOG_DEBUG("Rebuilding index for %s",
                                         gz_path.c_str());
                fs::remove(idx_path);
                auto new_indexer = IndexerFactory::create(
                    gz_path, idx_path, checkpoint_size, true);
                new_indexer->build();
            }
        }

        auto reader = ReaderFactory::create(gz_path, idx_path);
        std::size_t total_lines = reader->get_num_lines();

        DFTRACER_UTILS_LOG_DEBUG("File %s has %zu total lines", gz_path.c_str(),
                                 total_lines);

        LineCounter counter;
        reader->read_lines_with_processor(1, total_lines, counter);

        std::size_t event_count = counter.get_count();
        DFTRACER_UTILS_LOG_DEBUG("File %s: %zu events out of %zu lines",
                                 gz_path.c_str(), event_count, total_lines);

        return event_count;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error processing file %s: %s",
                                 gz_path.c_str(), e.what());
        return 0;
    }
}

std::size_t count_events_in_pfw_file(const std::string& pfw_path) {
    try {
        std::ifstream file(pfw_path);
        if (!file.is_open()) {
            DFTRACER_UTILS_LOG_ERROR("Cannot open file: %s", pfw_path.c_str());
            return 0;
        }

        LineCounter counter;
        std::string line;
        std::size_t line_count = 0;

        while (std::getline(file, line)) {
            line_count++;
            counter.process(line.c_str(), line.length());
        }

        std::size_t event_count = counter.get_count();
        DFTRACER_UTILS_LOG_DEBUG("File %s: %zu events out of %zu lines",
                                 pfw_path.c_str(), event_count, line_count);

        return event_count;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error processing file %s: %s",
                                 pfw_path.c_str(), e.what());
        return 0;
    }
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_event_count",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Count valid events in DFTracer .pfw or .pfw.gz files using pipeline "
        "processing");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-f", "--force").help("Force index recreation").flag();

    program.add_argument("-c", "--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--threads")
        .help(
            "Number of threads for parallel processing (default: number of CPU "
            "cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string log_dir = program.get<std::string>("--directory");
    bool force_rebuild = program.get<bool>("--force");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t num_threads = program.get<std::size_t>("--threads");
    std::string index_dir = program.get<std::string>("--index-dir");

    log_dir = fs::absolute(log_dir).string();

    std::vector<std::string> pfw_files;
    for (const auto& entry : fs::directory_iterator(log_dir)) {
        if (entry.is_regular_file()) {
            std::string path = entry.path().string();
            static constexpr std::string_view pfw_gz_suffix = ".pfw.gz";
            static constexpr std::string_view pfw_suffix = ".pfw";

            // Check for .pfw.gz files
            if (path.size() >= pfw_gz_suffix.size() &&
                path.compare(path.size() - pfw_gz_suffix.size(),
                             pfw_gz_suffix.size(), pfw_gz_suffix) == 0) {
                pfw_files.push_back(path);
            }
            // Check for .pfw files (but not .pfw.gz)
            else if (path.size() >= pfw_suffix.size() &&
                     path.compare(path.size() - pfw_suffix.size(),
                                  pfw_suffix.size(), pfw_suffix) == 0) {
                pfw_files.push_back(path);
            }
        }
    }

    if (pfw_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "No .pfw or .pfw.gz files found in directory: %s", log_dir.c_str());
        return 1;
    }

    DFTRACER_UTILS_LOG_DEBUG("Found %zu .pfw/.pfw.gz files to process",
                             pfw_files.size());
    DFTRACER_UTILS_LOG_DEBUG("Using %zu threads for processing", num_threads);

    auto start_time = std::chrono::high_resolution_clock::now();

    Pipeline pipeline;
    auto task_result = pipeline.add_task<std::vector<std::string>, std::size_t>(
        [checkpoint_size, force_rebuild, index_dir](
            std::vector<std::string> file_list,
            TaskContext& ctx) -> std::size_t {
            return process_files_parallel(file_list, checkpoint_size,
                                          force_rebuild, index_dir, ctx);
        });

    ThreadExecutor executor(num_threads);
    executor.execute(pipeline, pfw_files);
    std::size_t total_events = task_result.get();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    printf("%zu\n", total_events);

    DFTRACER_UTILS_LOG_DEBUG("Processed %zu files in %.2f ms", pfw_files.size(),
                             duration.count());
    DFTRACER_UTILS_LOG_DEBUG("Total valid events found: %zu", total_events);

    return 0;
}
