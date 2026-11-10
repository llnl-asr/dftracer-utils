// SPDX-License-Identifier: MIT
// Replay binary.
//
// DAG:
//   scan -> execute
//
// scan    : enumerate inputs (.pfw / .pfw.gz, directories recursively)
// execute : either ReplayEngine::run_pipelined (producer+consumer with
//           Channel<Trace> so read/parse latency is hidden behind the
//           consumer's apply_timing+execute) or, when --use-call-tree is
//           set, the legacy replay_with_call_tree path that builds a
//           hierarchical tree first

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/mpi/mpi_utils.h>
#include <dftracer/utils/utilities/replay/replay.h>

#ifdef DFTRACER_UTILS_MPI_ENABLED
#include <mpi.h>
#endif

#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/dstd/pair.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::replay;

namespace {

// Throws std::invalid_argument on a token that is not a 32-bit unsigned
// integer.
dstd::HashSet<std::uint32_t> parse_csv_uint32(dstd::StringView csv) {
    dstd::HashSet<std::uint32_t> out;
    for (const auto token : binaries::split_csv(csv)) {
        const auto value = dstd::from_chars<std::uint32_t>(token);
        if (!value) {
            throw std::invalid_argument(
                common::str_cat("\"", token, "\" is not an unsigned integer")
                    .c_str());
        }
        out.insert(*value);
    }
    return out;
}

class ReplayArgParse : public binaries::ArgParse {
   public:
    binaries::RuntimeArgs runtime_args;

    dstd::Vector<dstd::StringView> inputs;
    bool no_timing = false;
    bool dry_run = false;
    bool dftracer_mode = false;
    bool no_sleep = false;
    bool use_call_tree = false;
    bool hierarchical_replay = false;
    bool respect_call_hierarchy = false;

    dstd::StringView filter_pid_csv;
    dstd::StringView exclude_pid_csv;
    dstd::StringView filter_tid_csv;
    dstd::StringView exclude_tid_csv;
    dstd::StringView filter_function_csv;
    dstd::StringView exclude_function_csv;
    dstd::StringView filter_category_csv;
    dstd::StringView exclude_category_csv;

    double start_timestamp_us = 0;
    dstd::Optional<double> end_timestamp_us;
    std::uint64_t start_timestamp = 0;
    std::uint64_t end_timestamp = UINT64_MAX;
    std::int64_t min_size = -1;
    std::int64_t max_size = -1;
    double sample_rate = 1.0;
    std::uint64_t sample_seed = 0;
    std::size_t max_events = 0;
    std::size_t channel_capacity = 4096;

    explicit ReplayArgParse(binaries::ArgParser& p) : ArgParse(p) {
        schema(runtime_args);
    }

   protected:
    void register_args() override {
        auto& p = parser();

        p.positional(inputs, "inputs",
                     "Trace files (.pfw, .pfw.gz) or directories containing "
                     "trace files")
            .at_least_one();

        p.flag(no_timing, {"--no-timing"},
               "Ignore original timing and execute as fast as possible");
        p.flag(dry_run, {"--dry-run"},
               "Parse and analyze traces without executing operations");
        p.flag(dftracer_mode, {"--dftracer-mode"},
               "Use DFTracer sleep-based replay (sleep for operation "
               "duration instead of doing actual I/O)");
        p.flag(no_sleep, {"--no-sleep"},
               "When used with --dftracer-mode, disable sleep calls for "
               "maximum speed");
        p.flag(use_call_tree, {"--use-call-tree"},
               "Build and use call tree structure for hierarchical replay");
        p.flag(hierarchical_replay, {"--hierarchical-replay"},
               "Replay operations respecting parent-child call hierarchy "
               "(requires --use-call-tree)");
        p.flag(respect_call_hierarchy, {"--respect-call-hierarchy"},
               "Replay child nodes immediately after parent (requires "
               "--use-call-tree and --hierarchical-replay)");

        p.option(filter_pid_csv, {"--filter-pid"},
                 "Only replay events from specific PID(s) (comma-separated)");
        p.option(exclude_pid_csv, {"--exclude-pid"},
                 "Exclude events from specific PID(s) (comma-separated)");
        p.option(filter_tid_csv, {"--filter-tid"},
                 "Only replay events from specific TID(s) (comma-separated)");
        p.option(exclude_tid_csv, {"--exclude-tid"},
                 "Exclude events from specific TID(s) (comma-separated)");
        p.option(filter_function_csv, {"--filter-function"},
                 "Only replay specific function(s) (comma-separated, e.g., "
                 "'read,write,open')");
        p.option(exclude_function_csv, {"--exclude-function"},
                 "Exclude specific function(s) (comma-separated)");
        p.option(filter_category_csv, {"--filter-category"},
                 "Only replay specific category/categories (comma-separated, "
                 "e.g., 'POSIX,storage')");
        p.option(exclude_category_csv, {"--exclude-category"},
                 "Exclude specific category/categories (comma-separated)");

        p.duration(start_timestamp_us, 1e6, {"--start-timestamp"},
                   "Only replay events after this timestamp (a bare number is "
                   "microseconds; suffixed values like 5s are converted)",
                   "0");
        p.optional_duration(
            end_timestamp_us, 1e6, {"--end-timestamp"},
            "Only replay events before this timestamp (a bare number is "
            "microseconds; suffixed values like 5s are converted; empty = "
            "no limit)");
        p.bytes_signed(min_size, {"--min-size"},
                       "Only replay operations with size >= this value (-1 = "
                       "no limit). Accepts units, e.g. 4KB, 1MB",
                       "-1");
        p.bytes_signed(max_size, {"--max-size"},
                       "Only replay operations with size <= this value (-1 = "
                       "no limit). Accepts units, e.g. 4KB, 1MB",
                       "-1");

        p.option(sample_rate, {"--sample-rate"},
                 "Sample rate for replay (0.0-1.0, 1.0=all events, 0.1=10%)");
        p.option(sample_seed, {"--sample-seed"},
                 "Random seed for sampling (for reproducibility)");
        p.option(max_events, {"--max-events"},
                 "Maximum number of events to replay (0=unlimited)");
        p.option(channel_capacity, {"--channel-capacity"},
                 "Bounded Channel<Trace> capacity between read/parse producer "
                 "and dispatch consumer, at least 1 (default 4096)");
    }

    void post_parse() override {
        start_timestamp =
            static_cast<std::uint64_t>(std::llround(start_timestamp_us));
        end_timestamp =
            end_timestamp_us
                ? static_cast<std::uint64_t>(std::llround(*end_timestamp_us))
                : UINT64_MAX;
    }

    bool validate() override {
        if (channel_capacity == 0) {
            std::fprintf(stderr,
                         "Error: --channel-capacity must be at least 1\n");
            return false;
        }
        if (no_sleep && !dftracer_mode) {
            std::fprintf(stderr,
                         "Error: --no-sleep can only be used with "
                         "--dftracer-mode\n");
            return false;
        }
        if (hierarchical_replay && !use_call_tree) {
            std::fprintf(
                stderr,
                "Error: --hierarchical-replay requires --use-call-tree\n");
            return false;
        }
        if (respect_call_hierarchy && !hierarchical_replay) {
            std::fprintf(stderr,
                         "Error: --respect-call-hierarchy requires "
                         "--hierarchical-replay\n");
            return false;
        }
        if (sample_rate < 0.0 || sample_rate > 1.0) {
            std::fprintf(stderr,
                         "Error: --sample-rate must be between 0.0 and 1.0\n");
            return false;
        }
        const dstd::Pair<const char*, const dstd::StringView*> id_lists[] = {
            {"--filter-pid", &filter_pid_csv},
            {"--exclude-pid", &exclude_pid_csv},
            {"--filter-tid", &filter_tid_csv},
            {"--exclude-tid", &exclude_tid_csv}};
        for (const auto& [flag, csv] : id_lists) {
            try {
                (void)parse_csv_uint32(*csv);
            } catch (const std::invalid_argument& e) {
                std::fprintf(stderr, "Error: %s: %s\n", flag, e.what());
                return false;
            }
        }
        return true;
    }
};

dstd::HashSet<dstd::String> parse_csv_string(dstd::StringView csv) {
    dstd::HashSet<dstd::String> out;
    for (const auto token : binaries::split_csv(csv)) out.emplace(token);
    return out;
}

struct RunCtx {
    const ReplayArgParse* cli = nullptr;
    int mpi_rank = 0;
    int mpi_size = 1;
    bool is_root = true;

    ReplayConfig config;
    dstd::Vector<dstd::String> trace_files;
    ReplayResult result;
    int exit_code = 0;
    bool failed = false;

    double scan_ms = 0;
    double execute_ms = 0;
};

coro::CoroTask<void> task_scan(RunCtx* ctx, coro::Scope& scope) {
    const auto t0 = std::chrono::steady_clock::now();

    auto collected = co_await binaries::collect_input_trace_files(
        scope, ctx->cli->inputs, /*recursive=*/true, [](dstd::StringView in) {
            DFTRACER_UTILS_LOG_ERROR("Input not found or not accessible: {}",
                                     binaries::c_str(in));
        });
    for (const auto& file : collected) ctx->trace_files.emplace_back(file);

    ctx->scan_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();

    if (ctx->trace_files.empty()) {
        if (ctx->is_root) {
            std::fprintf(stderr,
                         "No trace files found in the specified inputs.\n");
        }
        ctx->failed = true;
        ctx->exit_code = 1;
        co_return;
    }

    if (ctx->is_root) {
        std::printf("Found %zu trace file(s) to replay:\n",
                    ctx->trace_files.size());
        for (const auto& file : ctx->trace_files) {
            std::printf("  %s\n", file.c_str());
        }
    }
    co_return;
}

void print_configuration(const RunCtx& ctx) {
    const auto& c = ctx.config;
    std::printf("\n=== Replay Configuration ===\n");
    if (ctx.mpi_size > 1) {
        std::printf("MPI processes: %d\n", ctx.mpi_size);
    }
    std::printf("Maintain timing: %s\n", c.maintain_timing ? "yes" : "no");
    std::printf("Dry run: %s\n", c.dry_run ? "yes" : "no");
    if (c.dftracer_mode) {
        std::printf("DFTracer mode: yes (%s)\n",
                    c.no_sleep ? "no-sleep" : "sleep-based");
    } else {
        std::printf("DFTracer mode: no (actual I/O)\n");
    }
    if (c.use_call_tree) {
        std::printf("Call tree mode: yes\n");
        std::printf("  Hierarchical replay: %s\n",
                    c.hierarchical_replay ? "yes" : "no");
        if (c.hierarchical_replay) {
            std::printf("  Respect call hierarchy: %s\n",
                        c.respect_call_hierarchy ? "yes" : "no");
        }
    }
}

void print_filters(const RunCtx& ctx) {
    const auto& c = ctx.config;
    bool any = !c.filter_pids.empty() || !c.exclude_pids.empty() ||
               !c.filter_tids.empty() || !c.exclude_tids.empty() ||
               !c.filter_functions.empty() || !c.exclude_functions.empty() ||
               !c.filter_categories.empty() || !c.exclude_categories.empty() ||
               c.start_timestamp > 0 || c.end_timestamp < UINT64_MAX ||
               c.min_operation_size >= 0 || c.max_operation_size >= 0 ||
               c.sampling_rate < 1.0 || c.max_events > 0;
    if (!any) return;

    std::printf("\nActive Filters:\n");
    auto print_uint_set = [](const char* label,
                             const dstd::HashSet<std::uint32_t>& s) {
        if (s.empty()) return;
        std::printf("  %s:", label);
        for (auto v : s) std::printf(" %u", v);
        std::printf("\n");
    };
    auto print_str_set = [](const char* label,
                            const dstd::HashSet<dstd::String>& s) {
        if (s.empty()) return;
        std::printf("  %s:", label);
        for (const auto& v : s) std::printf(" %s", v.c_str());
        std::printf("\n");
    };
    print_uint_set("Filter PIDs", c.filter_pids);
    print_uint_set("Exclude PIDs", c.exclude_pids);
    print_uint_set("Filter TIDs", c.filter_tids);
    print_uint_set("Exclude TIDs", c.exclude_tids);
    print_str_set("Filter functions", c.filter_functions);
    print_str_set("Exclude functions", c.exclude_functions);
    print_str_set("Filter categories", c.filter_categories);
    print_str_set("Exclude categories", c.exclude_categories);
    if (c.start_timestamp > 0)
        std::printf("  Start timestamp: %" PRIu64 "\n", c.start_timestamp);
    if (c.end_timestamp < UINT64_MAX)
        std::printf("  End timestamp: %" PRIu64 "\n", c.end_timestamp);
    if (c.min_operation_size >= 0)
        std::printf("  Min operation size: %" PRId64 " bytes\n",
                    c.min_operation_size);
    if (c.max_operation_size >= 0)
        std::printf("  Max operation size: %" PRId64 " bytes\n",
                    c.max_operation_size);
    if (c.sampling_rate < 1.0)
        std::printf("  Sampling rate: %g%%\n", c.sampling_rate * 100.0);
    if (c.max_events > 0) std::printf("  Max events: %zu\n", c.max_events);
}

coro::CoroTask<void> task_execute(RunCtx* ctx, coro::Scope* scope) {
    if (ctx->failed) co_return;

    const auto t0 = std::chrono::steady_clock::now();

    if (ctx->is_root) {
        print_configuration(*ctx);
        print_filters(*ctx);
        std::printf("\n=== Starting Replay ===\n");
    }

    const bool use_call_tree = ctx->config.use_call_tree;
    ReplayEngine engine(std::move(ctx->config));

    if (use_call_tree) {
        // Call tree path stays sync, replay_with_call_tree builds the
        // full tree in memory first, so there's nothing to hide behind a
        // channel. We just call it from inside this task.
        if (ctx->cli->inputs.size() != 1 ||
            !common::unwrap(
                dstd::fs::is_directory(dstd::PathView(ctx->cli->inputs[0])))) {
            if (ctx->is_root) {
                std::fprintf(stderr,
                             "Error: --use-call-tree requires exactly one "
                             "input directory\n");
            }
            ctx->failed = true;
            ctx->exit_code = 1;
            co_return;
        }
        if (ctx->is_root) {
            std::printf("Using call tree hierarchical replay mode\n");
        }
        ctx->result = engine.replay_with_call_tree(ctx->cli->inputs[0]);
    } else {
        // Pipelined path: producer (read+parse) feeds Channel<Trace>;
        // consumer (apply_timing+execute) drains it.
        auto replayed = co_await engine.run_pipelined(
            *scope, ctx->trace_files, ctx->result, ctx->cli->channel_capacity);
        if (!replayed) {
            std::fprintf(stderr, "Error: %s\n",
                         replayed.error().format().c_str());
            ctx->failed = true;
            ctx->exit_code = 1;
            co_return;
        }
    }

    ctx->execute_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

    if (ctx->is_root) {
        std::printf("\n=== Replay Completed ===\n");
        std::printf("Wall clock time: %.3f ms\n", ctx->execute_ms);
        ctx->result.print_summary();
    }

    // Exit-code semantics preserved from the previous binary:
    //   0 = at least one event executed and none failed
    //   1 = nothing executed (no events / no trace files)
    //   2 = at least one failed event
    if (ctx->result.failed_events > 0) {
        if (ctx->is_root) std::printf("\nReplay completed with errors.\n");
        ctx->exit_code = 2;
    } else if (ctx->result.executed_events > 0) {
        if (ctx->is_root) std::printf("\nReplay completed successfully.\n");
        ctx->exit_code = 0;
    } else {
        if (ctx->is_root) std::printf("\nNo events were executed.\n");
        ctx->exit_code = 1;
    }
    co_return;
}

int run(int argc, char** argv) {
    common::logger::init();
    if (auto configured = common::configure_memory_from_env(); !configured) {
        DFTRACER_UTILS_LOG_ERROR("Error: {}", configured.error().format());
        return 1;
    }

    binaries::ArgParser program(
        "dftracer_replay", DFTRACER_UTILS_PACKAGE_VERSION,
        "DFTracer replay utility - replays I/O operations from DFTracer "
        "trace files (.pfw, .pfw.gz)");

    ReplayArgParse cli(program);
    if (auto code = binaries::setup_and_parse(cli, argc, argv)) return *code;

    RunCtx ctx;
    ctx.cli = &cli;

#ifdef DFTRACER_UTILS_MPI_ENABLED
    ctx.mpi_rank = mpi::MPIUtils::instance().rank();
    ctx.mpi_size = mpi::MPIUtils::instance().world_size();
    ctx.is_root = mpi::MPIUtils::instance().is_root();
#endif

    // Mirror the parsed options into ReplayConfig.
    auto& c = ctx.config;
    c.maintain_timing = !cli.no_timing;
    c.dry_run = cli.dry_run;
    c.dftracer_mode = cli.dftracer_mode;
    c.no_sleep = cli.no_sleep;
    c.mpi_rank = ctx.mpi_rank;
    c.mpi_size = ctx.mpi_size;
    c.use_call_tree = cli.use_call_tree;
    c.hierarchical_replay = cli.hierarchical_replay;
    c.respect_call_hierarchy = cli.respect_call_hierarchy;
    c.filter_pids = parse_csv_uint32(cli.filter_pid_csv);
    c.exclude_pids = parse_csv_uint32(cli.exclude_pid_csv);
    c.filter_tids = parse_csv_uint32(cli.filter_tid_csv);
    c.exclude_tids = parse_csv_uint32(cli.exclude_tid_csv);
    c.filter_functions = parse_csv_string(cli.filter_function_csv);
    c.exclude_functions = parse_csv_string(cli.exclude_function_csv);
    c.filter_categories = parse_csv_string(cli.filter_category_csv);
    c.exclude_categories = parse_csv_string(cli.exclude_category_csv);
    c.start_timestamp = cli.start_timestamp;
    c.end_timestamp = cli.end_timestamp;
    c.min_operation_size = cli.min_size;
    c.max_operation_size = cli.max_size;
    c.sampling_rate = cli.sample_rate;
    c.sample_seed = cli.sample_seed;
    c.max_events = cli.max_events;

    RunCtx* ctx_ptr = &ctx;
    try {
        binaries::run_on_pool(
            "DFTracer Replay", cli.runtime_args,
            [ctx_ptr](coro::Scope&) -> coro::CoroTask<void> {
                co_await coro::with_scope([&](coro::Scope& scope) {
                    return task_scan(ctx_ptr, scope);
                });
                co_await coro::with_scope([&](coro::Scope& scope) {
                    return task_execute(ctx_ptr, &scope);
                });
            });
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Run failed: {}", e.what());
        return 1;
    }

    if (ctx.is_root) {
        DFTRACER_UTILS_LOG_DEBUG("[done] scan={:.1f}ms execute={:.1f}ms",
                                 ctx.scan_ms, ctx.execute_ms);
    }

    return ctx.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    MPI_Init(&argc, &argv);
    mpi::MPIUtils::instance().initialize();
#endif

    int rc = run(argc, argv);

#ifdef DFTRACER_UTILS_MPI_ENABLED
    mpi::MPIUtils::instance().finalize();
    MPI_Finalize();
#endif
    return rc;
}
