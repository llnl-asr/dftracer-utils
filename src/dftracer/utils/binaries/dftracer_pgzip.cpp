// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

using namespace dftracer::utils;

class PgzipArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultDot,
        "Folder of .pfw files (default: the current folder) or .pfw files"};
    binaries::RuntimeArgs runtime_args;
    binaries::WatchdogArgs watchdog;

    int compression_level = 6;
    std::size_t chunk_size = 4 * 1024 * 1024;

    explicit PgzipArgParse(binaries::ArgParser& p) : ArgParse(p) {
        schema(inputs, runtime_args, watchdog);
    }

   protected:
    void register_args() override {
        parser().option(compression_level, {"--compression-level"},
                        "Compression level (0-12, default: 6)");

        parser().bytes(
            chunk_size, {"--chunk-size"},
            "Chunk size for parallel compression (default: 4MB). Accepts "
            "units, e.g. 512KB, 4MB",
            "4194304");
    }
};

namespace {

struct FileResult {
    dstd::String input_path;
    dstd::String output_path;
    bool success = false;
    std::size_t original_size = 0;
    std::size_t compressed_size = 0;
    dstd::String error_message;
};

static coro::CoroTask<dstd::Result<void>> compress_file_body(
    FileResult& result, const dstd::String& file_path, int compression_level,
    std::size_t worker_count, std::size_t chunk_size) {
    auto checking = runtime::fs::exists(dstd::PathView(file_path));
    auto checked = co_await coro::or_fail(std::move(checking));
    if (!checked) {
        result.error_message = dstd::fmt<"File does not exist: {}">(file_path);
        co_return dstd::Result<void>();
    }

    auto input_stat = runtime::fs::stat(dstd::PathView(file_path));
    auto input_info = co_await coro::or_fail(std::move(input_stat));
    result.original_size = static_cast<std::size_t>(input_info.size);
    if (result.original_size == 0) {
        result.error_message = dstd::fmt<"Empty file: {}">(file_path);
        co_return dstd::Result<void>();
    }

    const dstd::OpenOptions open_options;
    auto opening =
        runtime::io::open_file(dstd::PathView(file_path), open_options);
    auto input = co_await std::move(opening);
    if (!input) {
        result.error_message = dstd::fmt<"Cannot open file: {}">(file_path);
        co_return dstd::Result<void>();
    }

    utilities::fileio::GzipWriterOptions opts;
    opts.member_size = chunk_size;
    opts.level = compression_level;
    opts.workers = worker_count;
    auto opening_writer = utilities::fileio::GzipLineWriter::open(
        dstd::String(result.output_path), std::move(opts));
    auto writer = co_await coro::or_fail(std::move(opening_writer));

    dstd::String buf;
    dstd::String carry;
    buf.resize(std::max<std::size_t>(chunk_size, 1 << 20));
    bool read_failed = false;
    std::uint64_t read_offset = 0;
    for (;;) {
        auto got = input->read_at(
            read_offset,
            dstd::Span<std::uint8_t>(
                reinterpret_cast<std::uint8_t*>(buf.data()), buf.size()));
        if (!got) {
            read_failed = true;
            break;
        }
        const std::size_t n = *got;
        if (n == 0) break;
        read_offset += n;
        dstd::StringView block(buf.data(), n);
        auto nl = block.rfind('\n');
        if (nl == dstd::StringView::npos) {
            carry.append(block);
            continue;
        }
        dstd::StringView whole = block.substr(0, nl + 1);
        if (carry.empty()) {
            auto appending = writer.append(whole);
            co_await coro::or_fail(std::move(appending));
        } else {
            carry.append(whole);
            auto appending = writer.append(carry);
            co_await coro::or_fail(std::move(appending));
            carry.clear();
        }
        carry.append(block.substr(nl + 1));
    }
    if (read_failed)
        co_return common::make_error<"read error: {}">(common::ErrorCode::Io,
                                                       file_path);

    auto closing = writer.close(carry);
    co_await coro::or_fail(std::move(closing));
    auto output_stat = runtime::fs::stat(dstd::PathView(result.output_path));
    auto output_info = co_await coro::or_fail(std::move(output_stat));
    result.compressed_size = static_cast<std::size_t>(output_info.size);
    result.success = true;
    co_return dstd::Result<void>();
}

static coro::CoroTask<FileResult> compress_file_parallel(
    const dstd::String& file_path, int compression_level,
    std::size_t worker_count, std::size_t chunk_size) {
    FileResult result;
    result.input_path = file_path;
    result.output_path = dstd::fmt<"{}.gz">(file_path);

    auto body = compress_file_body(result, file_path, compression_level,
                                   worker_count, chunk_size);
    auto outcome = co_await std::move(body);
    if (!outcome) {
        result.error_message =
            dstd::fmt<"Compression failed: {}">(outcome.error().c_str());
        auto removing = runtime::fs::remove(dstd::PathView(result.output_path));
        // Best effort: the compression error is already recorded.
        (co_await std::move(removing)).discard();
    }

    co_return result;
}

}  // namespace

static int run_pgzip(const PgzipArgParse& cli) {
    auto absolute_dir = binaries::absolute_path(cli.inputs.directory);
    if (!absolute_dir) return binaries::report_error(absolute_dir.error());
    const auto input_dir = std::move(*absolute_dir);
    const auto executor_threads = cli.runtime_args.executor_threads;
    const auto compression_level = cli.compression_level;
    const auto chunk_size = cli.chunk_size;

    dstd::Vector<dstd::String> input_files;
    if (!cli.inputs.directory.empty()) {
        auto dir = dstd::fs::Dir::open(dstd::PathView(input_dir));
        if (!dir) return binaries::report_error(dir.error());
        auto listing = dir->entries();
        if (!listing) return binaries::report_error(listing.error());
        for (const dstd::fs::DirEntry& entry : *listing) {
            if (dstd::PathView(entry.name).extension() != ".pfw") continue;
            const auto st = dir->stat(entry.name);
            if (!st || st->kind != dstd::fs::FileKind::File) continue;
            input_files.emplace_back(
                (dstd::PathView(input_dir) / entry.name).str());
        }
    }
    for (const auto file : cli.inputs.files) {
        if (dstd::PathView(file).extension() != ".pfw") continue;
        auto absolute = binaries::absolute_path(file);
        if (!absolute) return binaries::report_error(absolute.error());
        input_files.emplace_back(absolute->str());
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

    dstd::Vector<FileResult> results;
    std::mutex results_mutex;

    auto* files_ptr = &input_files;
    auto* results_ptr = &results;
    auto* mutex_ptr = &results_mutex;

    auto ran = binaries::run_on_pool(
        "DFTracer Parallel Gzip", cli.runtime_args, cli.watchdog,
        [files_ptr, results_ptr, mutex_ptr, compression_level, executor_threads,
         chunk_size](coro::Scope&) -> coro::CoroTask<void> {
            auto file_chan =
                coro::make_channel<std::size_t>(executor_threads * 2);

            auto compress_scope =
                [&file_chan, files_ptr, results_ptr, mutex_ptr,
                 compression_level, executor_threads,
                 chunk_size](coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn(
                    [ch = file_chan->producer(),
                     file_count =
                         files_ptr->size()]() mutable -> coro::CoroTask<void> {
                        auto guard = ch.guard();
                        for (std::size_t i = 0; i < file_count; ++i) {
                            const auto send_ok = co_await ch.send(i);
                            if (!send_ok) co_return;
                        }
                        co_return;
                    });

                for (std::size_t w = 0; w < executor_threads; ++w) {
                    scope.spawn([ch = file_chan->consumer(), files_ptr,
                                 results_ptr, mutex_ptr, compression_level,
                                 executor_threads,
                                 chunk_size]() -> coro::CoroTask<void> {
                        for (;;) {
                            auto fi_opt = co_await ch.receive();
                            if (!fi_opt) break;
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
                                const dstd::StringView name =
                                    dstd::PathView(path).filename();
                                DFTRACER_UTILS_LOG_DEBUG(
                                    "Compressed {}: {} -> {} bytes ({:.1f}%)",
                                    name, result.original_size,
                                    result.compressed_size, ratio);
                            }

                            if (result.success) {
                                auto removed =
                                    dstd::fs::remove(dstd::PathView(path));
                                if (!removed.has_value())
                                    DFTRACER_UTILS_LOG_ERROR(
                                        "Failed to remove {}: {}", path,
                                        removed.error().c_str());
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
            };
            co_await coro::with_scope(compress_scope);

            co_return;
        });
    if (!ran) return binaries::report_error(ran.error());

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
            DFTRACER_UTILS_LOG_ERROR("Failed to compress {}: {}", r.input_path,
                                     r.error_message);
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
    return binaries::cli_main<PgzipArgParse>(
        argc, argv, "dftracer_pgzip",
        "Parallel gzip compression for DFTracer .pfw files. "
        "Splits each file into chunks and compresses them in parallel "
        "as independent gzip members.",
        [](PgzipArgParse& cli) { return run_pgzip(cli); });
}
