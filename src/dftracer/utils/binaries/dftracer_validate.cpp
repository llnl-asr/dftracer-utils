#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using dftracer::utils::json_trim_and_validate;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;

class ValidateArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_EMPTY};
    cli::FilesArgs files_args{"Trace files to validate (.pfw, .pfw.gz)"};
    cli::PipelineArgs pipeline;

    explicit ValidateArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(directory, files_args, pipeline);
    }
};

namespace {

struct FileValResult {
    std::string path;
    std::size_t total_lines = 0;
    std::size_t invalid_lines = 0;
    std::size_t first_bad_line = 0;
    bool ok = true;
    std::string error;  // set when the file could not be read at all
};

// Explicit --files plus a recursive .pfw/.pfw.gz scan of the directory. The
// scanner is spawned on the scope so its recursive walk fans out per
// subdirectory (parallel readdir); sizes are not needed here.
coro::CoroTask<std::vector<std::string>> collect_files(
    CoroScope& ctx, std::string directory, std::vector<std::string> cli_files) {
    std::vector<std::string> files;
    for (auto& f : cli_files) {
        if (cli::is_trace_file(f)) files.push_back(std::move(f));
    }
    if (!directory.empty() && fs::exists(directory)) {
        auto scanned = co_await cli::scan_directory_trace_files(
            ctx, directory, /*recursive=*/true);
        files.insert(files.end(), std::make_move_iterator(scanned.begin()),
                     std::make_move_iterator(scanned.end()));
    }
    co_return files;
}

// Stream every line and confirm each non-wrapper line is valid JSON. Wrapper
// lines ("[" / "]"), blanks, and boundary artifacts are skipped, matching the
// pfw layout (leading "[" then one JSON object per line, no trailing comma).
coro::CoroTask<void> validate_file(std::string path, FileValResult* result) {
    result->path = std::move(path);
    try {
        TraceReaderConfig cfg;
        cfg.file_path = result->path;
        cfg.auto_build_index = false;
        TraceReader reader(cfg);
        // DOM (eager) parse: validates the whole line's JSON grammar. The
        // on-demand API is lazy and would accept malformed lines it never
        // navigates into.
        simdjson::dom::parser parser;
        auto gen = reader.read_lines(ReadConfig{});
        while (auto line_opt = co_await gen.next()) {
            const auto& line = *line_opt;
            const char* start = nullptr;
            std::size_t trimmed = 0;
            if (!json_trim_and_validate(line.content.data(),
                                        line.content.size(), start, trimmed)) {
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
    auto files = std::make_shared<std::vector<std::string>>();
    std::vector<FileValResult> results;
    const auto executor_threads = cli->pipeline.executor_threads;

    auto config =
        cli::build_pipeline_config("DFTracer Validate", cli->pipeline);
    Pipeline pipeline(config);

    // Task 1: discover files (parallel per-subdirectory scan + explicit
    // --files).
    auto collect_task = make_task(
        [cli, files](CoroScope& ctx) -> coro::CoroTask<void> {
            *files = co_await collect_files(ctx, cli->directory.value,
                                            cli->files_args.value);
            co_return;
        },
        "Collect");

    // Task 2: one worker per executor thread pulls file indices off a channel,
    // so files are validated in parallel.
    auto validate_task = make_task(
        [files, &results,
         executor_threads](CoroScope& ctx) -> coro::CoroTask<void> {
            if (files->empty()) co_return;
            results.resize(files->size());
            co_await ctx.scope([files, &results, executor_threads](
                                   CoroScope& scope) -> coro::CoroTask<void> {
                cli::parallel_for_each(
                    scope, std::views::iota(std::size_t{0}, files->size()),
                    executor_threads,
                    [files, &results](std::size_t idx) -> coro::CoroTask<void> {
                        co_await validate_file((*files)[idx], &results[idx]);
                    });
                co_return;
            });
            co_return;
        },
        "Validate");

    validate_task->depends_on(collect_task);
    pipeline.set_source(collect_task);
    pipeline.set_destination(validate_task);

    auto start_time = std::chrono::high_resolution_clock::now();
    pipeline.execute();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (results.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No .pfw or .pfw.gz files to validate (pass -d or --files)");
        co_return 1;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t total_lines = 0;
    for (const auto& r : results) {
        total_lines += r.total_lines;
        if (r.ok) {
            ++passed;
            DFTRACER_UTILS_LOG_DEBUG("valid: %s (%zu lines)", r.path.c_str(),
                                     r.total_lines);
        } else {
            ++failed;
            if (!r.error.empty()) {
                DFTRACER_UTILS_LOG_ERROR("unreadable: %s (%s)", r.path.c_str(),
                                         r.error.c_str());
            } else {
                DFTRACER_UTILS_LOG_ERROR(
                    "invalid: %s (%zu of %zu lines bad, first at line %zu)",
                    r.path.c_str(), r.invalid_lines, r.total_lines,
                    r.first_bad_line);
            }
        }
    }

    std::printf("Validated %zu file(s): %zu passed, %zu failed, %zu lines\n",
                results.size(), passed, failed, total_lines);
    DFTRACER_UTILS_LOG_DEBUG("Completed in %.2f ms", duration.count());

    co_return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    return cli::cli_main<ValidateArgParse>(
        argc, argv, "dftracer_validate",
        "Validate DFTracer .pfw / .pfw.gz trace files: every non-wrapper line "
        "must be valid JSON. Parallel C++ equivalent of the dftracer_validate "
        "script.",
        [](ValidateArgParse& cli) { return run_validate(&cli).get(); });
}
