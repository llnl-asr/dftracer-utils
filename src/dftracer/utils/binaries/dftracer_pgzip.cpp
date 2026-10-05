#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace dftracer::utils;
namespace fileio = dftracer::utils::utilities::fileio;

class PgzipArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_DOT,
                                 "Directory containing .pfw files"};
    cli::PipelineArgs pipeline;
    cli::WatchdogArgs watchdog;

    int compression_level = 6;
    std::size_t chunk_size = 4 * 1024 * 1024;

    explicit PgzipArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(directory, pipeline, watchdog);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-l", "--compression-level")
            .help("Compression level (0-12, default: 6)")
            .scan<'d', int>()
            .default_value(6);

        parser()
            .add_argument("--chunk-size")
            .help(
                "Chunk size for parallel compression (default: 4MB). Accepts "
                "units, e.g. 512KB, 4MB")
            .default_value(std::to_string(4 * 1024 * 1024));
    }

    void post_parse() override {
        compression_level = parser().get<int>("--compression-level");
        chunk_size = cli::get_bytes_arg(parser(), "--chunk-size");
    }
};

namespace {

struct FileResult {
    std::string input_path;
    std::string output_path;
    bool success = false;
    std::size_t original_size = 0;
    std::size_t compressed_size = 0;
    std::string error_message;
};

static coro::CoroTask<FileResult> compress_file_parallel(
    const std::string& file_path, int compression_level,
    std::size_t num_workers, std::size_t chunk_size) {
    FileResult result;
    result.input_path = file_path;
    result.output_path = file_path + ".gz";

    try {
        if (!fs::exists(file_path)) {
            result.error_message = "File does not exist: " + file_path;
            co_return result;
        }

        result.original_size = fs::file_size(file_path);
        if (result.original_size == 0) {
            result.error_message = "Empty file: " + file_path;
            co_return result;
        }

        std::ifstream ifs(file_path, std::ios::binary);
        if (!ifs.is_open()) {
            result.error_message = "Cannot open file: " + file_path;
            co_return result;
        }

        fileio::GzipWriterOptions opts;
        opts.member_size = chunk_size;
        opts.level = compression_level;
        opts.workers = num_workers;
        auto writer = unwrap(
            co_await fileio::GzipLineWriter::open(result.output_path, opts));

        std::string buf;
        std::string carry;
        buf.resize(std::max<std::size_t>(chunk_size, 1 << 20));
        while (ifs) {
            ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            auto n = static_cast<std::size_t>(ifs.gcount());
            if (n == 0) break;
            std::string_view block(buf.data(), n);
            auto nl = block.rfind('\n');
            if (nl == std::string_view::npos) {
                carry.append(block);
                continue;
            }
            std::string_view whole = block.substr(0, nl + 1);
            if (carry.empty()) {
                unwrap(co_await writer.append(whole));
            } else {
                carry.append(whole);
                unwrap(co_await writer.append(carry));
                carry.clear();
            }
            carry.append(block.substr(nl + 1));
        }
        if (ifs.bad()) throw std::runtime_error("read error: " + file_path);

        unwrap(co_await writer.close(carry));
        result.compressed_size = fs::file_size(result.output_path);
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Compression failed: ") + e.what();
        std::error_code ec;
        fs::remove(result.output_path, ec);
    }

    co_return result;
}

}  // namespace

static int run_pgzip(const PgzipArgParse& cli) {
    const auto input_dir = fs::absolute(cli.directory.value).string();
    const auto executor_threads = cli.pipeline.executor_threads;
    const auto compression_level = cli.compression_level;
    const auto chunk_size = cli.chunk_size;

    std::vector<std::string> input_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".pfw") {
            input_files.push_back(entry.path().string());
        }
    }

    if (input_files.empty()) {
        std::printf("No .pfw files found in %s\n", input_dir.c_str());
        return 0;
    }

    std::printf("==========================================\n");
    std::printf("DFTracer Parallel Gzip\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Input dir: %s\n", input_dir.c_str());
    std::printf("  Files: %zu\n", input_files.size());
    std::printf("  Compression level: %d\n", compression_level);
    std::printf("  Chunk size: %zu bytes\n", chunk_size);
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    auto pipeline_config = cli::build_pipeline_config(
        "DFTracer Parallel Gzip", cli.pipeline, cli.watchdog);
    Pipeline pipeline(pipeline_config);

    std::vector<FileResult> results;
    std::mutex results_mutex;

    auto* files_ptr = &input_files;
    auto* results_ptr = &results;
    auto* mutex_ptr = &results_mutex;

    auto compress_task = make_task(
        [files_ptr, results_ptr, mutex_ptr, compression_level, executor_threads,
         chunk_size](CoroScope& ctx) -> coro::CoroTask<void> {
            auto file_chan =
                coro::make_channel<std::size_t>(executor_threads * 2);

            co_await ctx.scope([&file_chan, files_ptr, results_ptr, mutex_ptr,
                                compression_level, executor_threads,
                                chunk_size](
                                   CoroScope& scope) -> coro::CoroTask<void> {
                scope.spawn(
                    [ch = file_chan->producer(), num_files = files_ptr->size()](
                        CoroScope&) mutable -> coro::CoroTask<void> {
                        auto guard = ch.guard();
                        for (std::size_t i = 0; i < num_files; ++i) {
                            if (!co_await ch.send(i)) co_return;
                        }
                        co_return;
                    });

                for (std::size_t w = 0; w < executor_threads; ++w) {
                    scope.spawn([ch = file_chan->consumer(), files_ptr,
                                 results_ptr, mutex_ptr, compression_level,
                                 executor_threads, chunk_size](
                                    CoroScope&) -> coro::CoroTask<void> {
                        while (auto fi_opt = co_await ch.receive()) {
                            const auto& path = (*files_ptr)[*fi_opt];

                            auto result = co_await compress_file_parallel(
                                path, compression_level, executor_threads,
                                chunk_size);

                            if (result.success) {
                                double ratio =
                                    result.original_size > 0
                                        ? static_cast<double>(
                                              result.compressed_size) /
                                              static_cast<double>(
                                                  result.original_size) *
                                              100.0
                                        : 0.0;
                                DFTRACER_UTILS_LOG_DEBUG(
                                    "Compressed %s: %zu -> %zu "
                                    "bytes (%.1f%%)",
                                    fs::path(path).filename().c_str(),
                                    result.original_size,
                                    result.compressed_size, ratio);
                            }

                            if (result.success) {
                                try {
                                    fs::remove(path);
                                } catch (const std::exception& e) {
                                    DFTRACER_UTILS_LOG_ERROR(
                                        "Failed to remove %s: %s", path.c_str(),
                                        e.what());
                                }
                            }

                            {
                                std::lock_guard<std::mutex> lock(*mutex_ptr);
                                results_ptr->push_back(std::move(result));
                            }
                        }
                        co_return;
                    });
                }

                co_return;
            });

            co_return;
        },
        "ParallelGzip");

    pipeline.set_source(compress_task);
    pipeline.set_destination(compress_task);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::size_t successful = 0;
    std::size_t total_original = 0;
    std::size_t total_compressed = 0;

    for (const auto& r : results) {
        if (r.success) {
            successful++;
            total_original += r.original_size;
            total_compressed += r.compressed_size;
        } else {
            DFTRACER_UTILS_LOG_ERROR("Failed to compress %s: %s",
                                     r.input_path.c_str(),
                                     r.error_message.c_str());
        }
    }

    double overall_ratio = total_original > 0
                               ? static_cast<double>(total_compressed) /
                                     static_cast<double>(total_original) * 100.0
                               : 0.0;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Gzip Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Processed: %zu/%zu files\n", successful, input_files.size());
    std::printf("  Total: %zu -> %zu bytes (%.1f%% compression ratio)\n",
                total_original, total_compressed, overall_ratio);
    std::printf("==========================================\n");

    return successful == input_files.size() ? 0 : 1;
}

int main(int argc, char** argv) {
    return cli::cli_main<PgzipArgParse>(
        argc, argv, "dftracer_pgzip",
        "Parallel gzip compression for DFTracer .pfw files. "
        "Splits each file into chunks and compresses them in parallel "
        "as independent gzip members.",
        [](PgzipArgParse& cli) { return run_pgzip(cli); });
}
