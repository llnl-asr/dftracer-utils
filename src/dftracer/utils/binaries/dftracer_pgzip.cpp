#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/compression/zlib/types.h>
#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;

class PgzipArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_DOT,
                                 "Directory containing .pfw files"};
    cli::PipelineArgs pipeline;
    cli::WatchdogArgs watchdog;

    int compression_level = Z_DEFAULT_COMPRESSION;
    std::size_t chunk_size = 4 * 1024 * 1024;

    explicit PgzipArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(directory, pipeline, watchdog);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-l", "--compression-level")
            .help("Compression level (0-9, default: Z_DEFAULT_COMPRESSION)")
            .scan<'d', int>()
            .default_value(Z_DEFAULT_COMPRESSION);

        parser()
            .add_argument("--chunk-size")
            .help("Chunk size in bytes for parallel compression (default: 4MB)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(4 * 1024 * 1024));
    }

    void post_parse() override {
        compression_level = parser().get<int>("--compression-level");
        chunk_size = parser().get<std::size_t>("--chunk-size");
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

struct ChunkWork {
    std::size_t index;
    std::string data;
};

struct CompressedChunk {
    std::size_t index;
    std::string data;
};

static coro::CoroTask<void> chunk_reader(
    coro::ChannelProducer<ChunkWork> producer, const std::string* file_path,
    std::size_t chunk_size) {
    auto guard = producer.guard();

    std::ifstream ifs(*file_path, std::ios::binary);
    if (!ifs.is_open()) co_return;

    std::string buffer(chunk_size, '\0');
    std::size_t idx = 0;

    while (ifs) {
        ifs.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
        auto bytes_read = static_cast<std::size_t>(ifs.gcount());
        if (bytes_read == 0) break;

        ChunkWork work;
        work.index = idx++;
        work.data.assign(buffer.data(), bytes_read);

        if (!co_await producer.send(std::move(work))) break;
    }
    co_return;
}

static coro::CoroTask<void> chunk_compressor(
    coro::ChannelConsumer<ChunkWork> input_chan,
    coro::ChannelProducer<CompressedChunk> out_producer,
    int compression_level) {
    auto guard = out_producer.guard();

    z_stream strm{};
    using dftracer::utils::utilities::compression::zlib::CompressionFormat;
    int rc = deflateInit2(&strm, compression_level, Z_DEFLATED,
                          static_cast<int>(CompressionFormat::GZIP), 8,
                          Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) co_return;

    std::string out_buf(64 * 1024, '\0');

    while (auto work = co_await input_chan.receive()) {
        std::string compressed;
        compressed.reserve(work->data.size());

        strm.next_in =
            reinterpret_cast<Bytef*>(const_cast<char*>(work->data.data()));
        strm.avail_in = static_cast<uInt>(work->data.size());

        do {
            strm.next_out = reinterpret_cast<Bytef*>(out_buf.data());
            strm.avail_out = static_cast<uInt>(out_buf.size());

            rc = deflate(&strm, Z_FINISH);

            std::size_t have = out_buf.size() - strm.avail_out;
            if (have > 0) {
                compressed.append(out_buf.data(),
                                  static_cast<std::ptrdiff_t>(have));
            }
        } while (rc == Z_OK);

        deflateReset(&strm);

        CompressedChunk result;
        result.index = work->index;
        result.data = std::move(compressed);

        if (!co_await out_producer.send(std::move(result))) break;
    }

    deflateEnd(&strm);
    co_return;
}

static coro::CoroTask<void> chunk_writer(
    coro::ChannelConsumer<CompressedChunk> output_chan,
    const std::string* output_path) {
    std::ofstream ofs(*output_path, std::ios::binary);
    if (!ofs.is_open()) co_return;

    std::size_t next_expected = 0;
    std::map<std::size_t, std::string> pending;

    while (auto chunk = co_await output_chan.receive()) {
        if (chunk->index == next_expected) {
            ofs.write(chunk->data.data(),
                      static_cast<std::streamsize>(chunk->data.size()));
            ++next_expected;

            while (true) {
                auto it = pending.find(next_expected);
                if (it == pending.end()) break;
                ofs.write(it->second.data(),
                          static_cast<std::streamsize>(it->second.size()));
                pending.erase(it);
                ++next_expected;
            }
        } else {
            pending.emplace(chunk->index, std::move(chunk->data));
        }
    }

    ofs.close();
    co_return;
}

static coro::CoroTask<FileResult> compress_file_parallel(
    CoroScope& ctx, const std::string& file_path, int compression_level,
    std::size_t num_workers, std::size_t chunk_size) {
    FileResult result;
    result.input_path = file_path;
    result.output_path = file_path + ".gz";

    try {
        if (!fs::exists(file_path)) {
            result.error_message = "File does not exist: " + file_path;
            co_return result;
        }

        auto file_size = fs::file_size(file_path);
        result.original_size = file_size;

        if (file_size == 0) {
            result.error_message = "Empty file: " + file_path;
            co_return result;
        }

        if (file_size <= chunk_size) {
            num_workers = 1;
        }

        auto input_chan = coro::make_channel<ChunkWork>(num_workers * 2);
        auto output_chan = coro::make_channel<CompressedChunk>(num_workers * 2);

        const auto* file_path_ptr = &file_path;
        const auto* output_path_ptr = &result.output_path;

        co_await ctx.scope([&input_chan, &output_chan, file_path_ptr,
                            output_path_ptr, compression_level, num_workers,
                            chunk_size](
                               CoroScope& scope) -> coro::CoroTask<void> {
            scope.spawn([ch = input_chan->producer(), file_path_ptr,
                         chunk_size](
                            CoroScope&) mutable -> coro::CoroTask<void> {
                co_await chunk_reader(std::move(ch), file_path_ptr, chunk_size);
            });

            for (std::size_t w = 0; w < num_workers; ++w) {
                scope.spawn([in_ch = input_chan->consumer(),
                             out_ch = output_chan->producer(),
                             compression_level](
                                CoroScope&) mutable -> coro::CoroTask<void> {
                    co_await chunk_compressor(in_ch, std::move(out_ch),
                                              compression_level);
                });
            }

            scope.spawn([ch = output_chan->consumer(),
                         output_path_ptr](CoroScope&) -> coro::CoroTask<void> {
                co_await chunk_writer(ch, output_path_ptr);
            });

            co_return;
        });

        if (fs::exists(result.output_path)) {
            result.compressed_size = fs::file_size(result.output_path);
            result.success = true;
        } else {
            result.error_message = "Output file not created";
        }

    } catch (const std::exception& e) {
        result.error_message = std::string("Compression failed: ") + e.what();
        if (fs::exists(result.output_path)) {
            try {
                fs::remove(result.output_path);
            } catch (...) {
            }
        }
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
                                    CoroScope& wctx) -> coro::CoroTask<void> {
                        while (auto fi_opt = co_await ch.receive()) {
                            const auto& path = (*files_ptr)[*fi_opt];

                            auto result = co_await compress_file_parallel(
                                wctx, path, compression_level, executor_threads,
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
