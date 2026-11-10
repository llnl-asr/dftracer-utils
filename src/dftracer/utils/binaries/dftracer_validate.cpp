// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <simdjson.h>

#include <chrono>
#include <cstdio>
#include <ranges>

using namespace dftracer::utils;
using dftracer::utils::index::gzip::CheckpointIndexerFactory;
using dftracer::utils::json::trim_and_validate;
using dftracer::utils::utilities::fileio::lines::sources::
    async_streaming_gz_lines;

class ValidateArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{binaries::DirMode::Required,
                               "Trace files (.pfw.gz) or a folder of them"};
    binaries::RuntimeArgs runtime_args;

    explicit ValidateArgParse(binaries::ArgParser& p) : ArgParse(p) {
        schema(inputs, runtime_args);
    }
};

namespace {

struct FileValResult {
    dstd::String path;
    std::size_t total_lines = 0;
    std::size_t invalid_lines = 0;
    std::size_t first_bad_line = 0;
    bool ok = true;
    dstd::String error;  // set when the file could not be read at all
};

// The file inputs plus a recursive trace file scan of the directory. The
// scanner is spawned on the scope so its recursive walk fans out per
// subdirectory (parallel readdir); sizes are not needed here.
coro::CoroTask<dstd::Vector<dstd::String>> collect_files(
    coro::Scope& ctx, dstd::StringView directory,
    const dstd::Vector<dstd::StringView>* cli_files) {
    dstd::Vector<dstd::String> files;
    for (const auto f : *cli_files) files.emplace_back(f);
    if (!directory.empty()) {
        auto scanned = co_await binaries::scan_directory_trace_files(
            ctx, directory, /*recursive=*/true);
        files.insert(files.end(), std::make_move_iterator(scanned.begin()),
                     std::make_move_iterator(scanned.end()));
    }
    co_return files;
}

// Stream every line and confirm each non-wrapper line is valid JSON. Wrapper
// lines ("[" / "]"), blanks, and boundary artifacts are skipped, matching the
// pfw layout (leading "[" then one JSON object per line, no trailing comma).
coro::CoroTask<void> validate_file(dstd::String path, FileValResult* result) {
    result->path = std::move(path);
    try {
        // Stream the gzip directly, never through TraceReader: its indexed
        // read path serves a byte range from a `.dftindex`, and a file whose
        // entry is absent from a shared root index resolves to zero bytes, so
        // validate would read no lines and still count the file as passing
        // without ever decompressing it.
        const dstd::String std_path(result->path);
        if (CheckpointIndexerFactory::detect_format(std_path) !=
            common::ArchiveFormat::Gzip) {
            throw std::runtime_error(
                "not a gzip trace (dftracer traces must be gzip-compressed)");
        }
        // DOM (eager) parse: validates the whole line's JSON grammar. The
        // on-demand API is lazy and would accept malformed lines it never
        // navigates into.
        simdjson::dom::parser parser;
        // A cut trace is invalid here, not recovered.
        auto gen = async_streaming_gz_lines(result->path, 0, 0,
                                            /*recover_truncated=*/false);
        for (;;) {
            auto line_opt = co_await gen.next();
            if (!line_opt) break;
            if (!*line_opt) common::throw_error(line_opt->error());
            const auto& line = **line_opt;
            const char* start = nullptr;
            std::size_t trimmed = 0;
            if (!trim_and_validate(line.content.data(), line.content.size(),
                                   start, trimmed)) {
                continue;
            }
            ++result->total_lines;
            if (parser.parse(start, trimmed).error() != simdjson::SUCCESS) {
                ++result->invalid_lines;
                if (result->first_bad_line == 0) {
                    result->first_bad_line = line.line_number;
                }
            }
        }
    } catch (const std::exception& e) {
        result->error = e.what();
    }
    result->ok = result->error.empty() && result->invalid_lines == 0;
    co_return;
}

}  // namespace

static coro::CoroTask<int> run_validate(const ValidateArgParse* cli) {
    auto files = dstd::make_shared<dstd::Vector<dstd::String>>();
    dstd::Vector<FileValResult> results;
    const auto executor_threads = cli->runtime_args.executor_threads;

    auto start_time = std::chrono::high_resolution_clock::now();
    binaries::run_on_pool(
        "DFTracer Validate", cli->runtime_args,
        [cli, files, &results,
         executor_threads](coro::Scope& scope) -> coro::CoroTask<void> {
            *files = co_await collect_files(scope, cli->inputs.directory,
                                            &cli->inputs.files);
            if (files->empty()) co_return;
            results.resize(files->size());
            binaries::parallel_for_each(
                scope, std::views::iota(std::size_t{0}, files->size()),
                executor_threads,
                [files, &results](std::size_t idx) -> coro::CoroTask<void> {
                    co_await validate_file((*files)[idx], &results[idx]);
                });
            co_return;
        });
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (results.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}", "No trace files to validate (pass -d or --files)");
        co_return 1;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t total_lines = 0;
    for (const auto& r : results) {
        total_lines += r.total_lines;
        if (r.ok) {
            ++passed;
            DFTRACER_UTILS_LOG_DEBUG("valid: {} ({} lines)", r.path,
                                     r.total_lines);
        } else {
            ++failed;
            if (!r.error.empty()) {
                DFTRACER_UTILS_LOG_ERROR("unreadable: {} ({})", r.path,
                                         r.error);
            } else {
                DFTRACER_UTILS_LOG_ERROR(
                    "invalid: {} ({} of {} lines bad, first at line {})",
                    r.path, r.invalid_lines, r.total_lines, r.first_bad_line);
            }
        }
    }

    std::printf("Validated %zu file(s): %zu passed, %zu failed, %zu lines\n",
                results.size(), passed, failed, total_lines);
    DFTRACER_UTILS_LOG_DEBUG("Completed in {:.2f} ms", duration.count());

    co_return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    return binaries::cli_main<ValidateArgParse>(
        argc, argv, "dftracer_validate",
        "Validate DFTracer .pfw.gz trace files: every non-wrapper line "
        "must be valid JSON. Parallel C++ equivalent of the dftracer_validate "
        "script.",
        [](ValidateArgParse& cli) { return run_validate(&cli).get(); });
}
