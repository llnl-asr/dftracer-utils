#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task_tag.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/line_processor.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <zlib.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace dftracer::utils;

struct MergeResult {
    std::string file_path;
    std::string temp_file_path;
    bool success;
    std::size_t lines_processed;
    std::size_t valid_events;
};

class TempFileLineProcessor : public LineProcessor {
   private:
    FILE* temp_file_;
    std::atomic<std::size_t> valid_events_{0};
    std::atomic<std::size_t> total_lines_{0};

   public:
    TempFileLineProcessor(const std::string& temp_path) {
        temp_file_ = std::fopen(temp_path.c_str(), "w");
        if (temp_file_) {
            setvbuf(temp_file_, nullptr, _IOFBF, 1024 * 1024);
        }
    }

    ~TempFileLineProcessor() {
        if (temp_file_) {
            std::fclose(temp_file_);
        }
    }

    bool process(const char* data, std::size_t length) override {
        total_lines_++;

        if (!temp_file_) {
            return true;
        }

        const char* trimmed;
        std::size_t trimmed_length;

        if (json_trim_and_validate(data, length, trimmed, trimmed_length) &&
            trimmed_length > 8) {
            std::fwrite(trimmed, 1, trimmed_length, temp_file_);
            std::fwrite("\n", 1, 1, temp_file_);
            valid_events_++;
        }

        return true;
    }

    bool is_valid() const { return temp_file_ != nullptr; }
    std::size_t get_valid_events() const { return valid_events_.load(); }
    std::size_t get_total_lines() const { return total_lines_.load(); }
};

static MergeResult process_pfw_gz_file(const std::string& gz_path,
                                       const std::string& idx_path,
                                       std::size_t checkpoint_size,
                                       const std::string& temp_dir,
                                       bool force_rebuild, TaskContext&) {
    // Generate unique temp file name
    static std::atomic<int> temp_counter{0};
    std::string temp_file =
        temp_dir + "/merge_temp_" + std::to_string(temp_counter++) + ".tmp";

    MergeResult result{gz_path, temp_file, false, 0, 0};

    try {
        // Build or load index
        if (!fs::exists(idx_path) || force_rebuild) {
            if (force_rebuild && fs::exists(idx_path)) {
                fs::remove(idx_path);
            }
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

        TempFileLineProcessor processor(temp_file);
        if (!processor.is_valid()) {
            DFTRACER_UTILS_LOG_ERROR("Failed to create temp file: %s",
                                     temp_file.c_str());
            return result;
        }

        reader->read_lines_with_processor(1, total_lines, processor);

        result.lines_processed = processor.get_total_lines();
        result.valid_events = processor.get_valid_events();
        result.success = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "Processed %s: %zu valid events from %zu lines", gz_path.c_str(),
            result.valid_events, result.lines_processed);

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_DEBUG("Error processing file %s: %s",
                                 gz_path.c_str(), e.what());
    }

    return result;
}

static MergeResult process_pfw_file(const std::string& pfw_path,
                                    const std::string& temp_dir, TaskContext&) {
    static std::atomic<int> temp_counter{0};
    std::string temp_file =
        temp_dir + "/merge_temp_" + std::to_string(temp_counter++) + ".tmp";

    MergeResult result{pfw_path, temp_file, false, 0, 0};

    try {
        std::ifstream file(pfw_path);
        if (!file.is_open()) {
            DFTRACER_UTILS_LOG_ERROR("Cannot open file: %s", pfw_path.c_str());
            return result;
        }

        TempFileLineProcessor processor(temp_file);
        if (!processor.is_valid()) {
            DFTRACER_UTILS_LOG_ERROR("Failed to create temp file: %s",
                                     temp_file.c_str());
            return result;
        }

        std::string line;
        while (std::getline(file, line)) {
            processor.process(line.c_str(), line.length());
        }

        result.lines_processed = processor.get_total_lines();
        result.valid_events = processor.get_valid_events();
        result.success = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "Processed %s: %zu valid events from %zu lines", pfw_path.c_str(),
            result.valid_events, result.lines_processed);

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_DEBUG("Error processing file %s: %s",
                                 pfw_path.c_str(), e.what());
    }

    return result;
}

static std::vector<MergeResult> process_files_parallel(
    const std::vector<std::string>& files, std::size_t checkpoint_size,
    bool force_rebuild, const std::string& temp_dir,
    const std::string& index_dir, TaskContext& p_ctx) {
    std::vector<TaskResult<MergeResult>::Future> futures;
    futures.reserve(files.size());

    auto process_file = [&temp_dir, &index_dir, checkpoint_size, force_rebuild](
                            std::string file_path,
                            TaskContext& ctx) -> MergeResult {
        const std::string pfw_gz_suffix = ".pfw.gz";
        const std::string pfw_suffix = ".pfw";

        if (file_path.size() >= pfw_gz_suffix.size() &&
            file_path.compare(file_path.size() - pfw_gz_suffix.size(),
                              pfw_gz_suffix.size(), pfw_gz_suffix) == 0) {
            std::string idx_path =
                utilities::composites::dft::determine_index_path(file_path,
                                                                 index_dir);
            return process_pfw_gz_file(file_path, idx_path, checkpoint_size,
                                       temp_dir, force_rebuild, ctx);
        } else if (file_path.size() >= pfw_suffix.size() &&
                   file_path.compare(file_path.size() - pfw_suffix.size(),
                                     pfw_suffix.size(), pfw_suffix) == 0) {
            // Handle plain .pfw files
            return process_pfw_file(file_path, temp_dir, ctx);
        } else {
            DFTRACER_UTILS_LOG_DEBUG("Unknown file type: %s",
                                     file_path.c_str());
            return MergeResult{file_path, "", false, 0, 0};
        }
    };

    for (const auto& file_path : files) {
        auto task_result = p_ctx.emit<std::string, MergeResult>(
            process_file, Input{file_path});
        futures.push_back(std::move(task_result.future()));
    }

    std::vector<MergeResult> results;
    results.reserve(files.size());

    for (auto& future : futures) {
        results.push_back(future.get());
    }

    return results;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_merge",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Merge DFTracer .pfw or .pfw.gz files into a single JSON array file "
        "using pipeline processing");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-o", "--output")
        .help("Output file path (should have .pfw extension)")
        .default_value<std::string>("combined.pfw");

    program.add_argument("-f", "--force")
        .help("Override existing output file and force index recreation")
        .flag();

    program.add_argument("-c", "--compress")
        .help("Compress output file with gzip")
        .flag();

    program.add_argument("-v", "--verbose").help("Enable verbose mode").flag();

    program.add_argument("-g", "--gzip-only")
        .help("Process only .pfw.gz files")
        .flag();

    program.add_argument("--checkpoint-size")
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
        std::cerr << program << std::endl;
        return 1;
    }

    std::string input_dir = program.get<std::string>("--directory");
    std::string output_file = program.get<std::string>("--output");
    bool force_override = program.get<bool>("--force");
    bool compress_output = program.get<bool>("--compress");
    bool verbose = program.get<bool>("--verbose");
    bool gzip_only = program.get<bool>("--gzip-only");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t num_threads = program.get<std::size_t>("--threads");
    std::string index_dir = program.get<std::string>("--index-dir");

    input_dir = fs::absolute(input_dir).string();
    output_file = fs::absolute(output_file).string();

    // Validate output file extension
    if (output_file.size() < 4 ||
        output_file.substr(output_file.size() - 4) != ".pfw") {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Output file should have .pfw extension");
        return 1;
    }

    std::string final_output =
        compress_output ? output_file + ".gz" : output_file;
    if (fs::exists(final_output) && !force_override) {
        DFTRACER_UTILS_LOG_ERROR(
            "Output file %s exists and force override is disabled",
            final_output.c_str());
        return 1;
    }

    if (force_override) {
        if (fs::exists(output_file)) fs::remove(output_file);
        if (fs::exists(output_file + ".gz")) fs::remove(output_file + ".gz");
    }

    std::vector<std::string> input_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file()) {
            std::string path = entry.path().string();
            const std::string pfw_gz_suffix = ".pfw.gz";
            const std::string pfw_suffix = ".pfw";

            if (path.size() >= pfw_gz_suffix.size() &&
                path.compare(path.size() - pfw_gz_suffix.size(),
                             pfw_gz_suffix.size(), pfw_gz_suffix) == 0) {
                input_files.push_back(path);
            } else if (!gzip_only && path.size() >= pfw_suffix.size() &&
                       path.compare(path.size() - pfw_suffix.size(),
                                    pfw_suffix.size(), pfw_suffix) == 0) {
                input_files.push_back(path);
            }
        }
    }

    if (input_files.empty()) {
        const char* file_types = gzip_only ? ".pfw.gz" : ".pfw or .pfw.gz";
        DFTRACER_UTILS_LOG_ERROR("No %s files found in directory: %s",
                                 file_types, input_dir.c_str());
        return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu files to merge", input_files.size());
    if (verbose) {
        DFTRACER_UTILS_LOG_INFO("Using %zu threads for processing",
                                num_threads);
        DFTRACER_UTILS_LOG_INFO("Output file: %s", final_output.c_str());
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::string temp_dir =
        fs::path(output_file).parent_path() / "dftracer_merge_tmp";
    fs::create_directories(temp_dir);

    Pipeline pipeline;
    auto task_result =
        pipeline.add_task<std::vector<std::string>, std::vector<MergeResult>>(
            [temp_dir, checkpoint_size, force_override, index_dir](
                std::vector<std::string> file_list,
                TaskContext& ctx) -> std::vector<MergeResult> {
                return process_files_parallel(file_list, checkpoint_size,
                                              force_override, temp_dir,
                                              index_dir, ctx);
            });

    ThreadExecutor executor(num_threads);
    executor.execute(pipeline, input_files);
    std::vector<MergeResult> results = task_result.get();

    FILE* output_fp = std::fopen(output_file.c_str(), "w");
    if (!output_fp) {
        DFTRACER_UTILS_LOG_ERROR("Cannot create output file: %s",
                                 output_file.c_str());
        return 1;
    }

    setvbuf(output_fp, nullptr, _IOFBF, 1024 * 1024);

    std::fprintf(output_fp, "[\n");

    constexpr std::size_t BUFFER_SIZE = 64 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    for (const auto& result : results) {
        if (result.success && !result.temp_file_path.empty()) {
            FILE* temp_fp = std::fopen(result.temp_file_path.c_str(), "r");
            if (temp_fp) {
                std::size_t bytes_read;
                while ((bytes_read = std::fread(buffer.data(), 1, BUFFER_SIZE,
                                                temp_fp)) > 0) {
                    std::fwrite(buffer.data(), 1, bytes_read, output_fp);
                }
                std::fclose(temp_fp);
                fs::remove(result.temp_file_path);
            }
        }
    }

    std::fprintf(output_fp, "\n]\n");
    std::fclose(output_fp);

    fs::remove_all(temp_dir);

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::size_t successful_files = 0;
    std::size_t total_events = 0;
    std::size_t total_lines = 0;

    for (const auto& result : results) {
        if (result.success) {
            successful_files++;
            total_events += result.valid_events;
            total_lines += result.lines_processed;
        } else {
            DFTRACER_UTILS_LOG_DEBUG("Failed to process: %s",
                                     result.file_path.c_str());
        }
    }

    if (compress_output && successful_files > 0) {
        DFTRACER_UTILS_LOG_INFO("%s", "Compressing output file...");

        std::ifstream infile(output_file, std::ios::binary);
        std::ofstream outfile(output_file + ".gz", std::ios::binary);

        if (infile && outfile) {
            z_stream strm{};
            if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16,
                             8, Z_DEFAULT_STRATEGY) == Z_OK) {
                constexpr std::size_t COMPRESS_BUFFER_SIZE = 64 * 1024;
                std::vector<unsigned char> in_buffer(COMPRESS_BUFFER_SIZE);
                std::vector<unsigned char> out_buffer(COMPRESS_BUFFER_SIZE);

                int flush = Z_NO_FLUSH;
                do {
                    infile.read(reinterpret_cast<char*>(in_buffer.data()),
                                COMPRESS_BUFFER_SIZE);
                    std::streamsize bytes_read = infile.gcount();

                    if (bytes_read == 0) break;

                    strm.avail_in = static_cast<uInt>(bytes_read);
                    strm.next_in = in_buffer.data();
                    flush = infile.eof() ? Z_FINISH : Z_NO_FLUSH;

                    do {
                        strm.avail_out = COMPRESS_BUFFER_SIZE;
                        strm.next_out = out_buffer.data();
                        deflate(&strm, flush);

                        std::size_t bytes_to_write =
                            COMPRESS_BUFFER_SIZE - strm.avail_out;
                        outfile.write(
                            reinterpret_cast<const char*>(out_buffer.data()),
                            bytes_to_write);
                    } while (strm.avail_out == 0);
                } while (flush != Z_FINISH);

                deflateEnd(&strm);
                infile.close();
                outfile.close();

                if (fs::exists(output_file + ".gz")) {
                    fs::remove(output_file);
                    DFTRACER_UTILS_LOG_INFO("Created compressed output: %s",
                                            (output_file + ".gz").c_str());
                }
            }
        }
    }

    printf("Merge completed. Processed %zu/%zu files in %.2f ms\n",
           successful_files, results.size(), duration.count());
    printf("Total: %zu valid events from %zu lines\n", total_events,
           total_lines);

    return successful_files == results.size() ? 0 : 1;
}
