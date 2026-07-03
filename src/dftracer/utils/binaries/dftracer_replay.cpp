// Pipeline-driven replay binary.
//
// DAG:
//   scan -> execute
//
// scan    : enumerate inputs (.pfw / .pfw.gz, recursive optional)
// execute : either ReplayEngine::run_pipelined (producer+consumer with
//           Channel<Trace> so read/parse latency is hidden behind the
//           consumer's apply_timing+execute) or, when --use-call-tree is
//           set, the legacy replay_with_call_tree path that builds a
//           hierarchical tree first

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/mpi/mpi_utils.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/replay/replay.h>

#include <argparse/argparse.hpp>

#ifdef DFTRACER_UTILS_MPI_ENABLED
#include <mpi.h>
#endif

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::replay;

namespace {

class ReplayArgParse : public cli::ArgParse {
   public:
    cli::PipelineArgs pipeline;

    std::vector<std::string> inputs;
    bool no_timing = false;
    bool dry_run = false;
    bool dftracer_mode = false;
    bool no_sleep = false;
    bool recursive = false;
    bool use_call_tree = false;
    bool hierarchical_replay = false;
    bool respect_call_hierarchy = false;

    std::string filter_pid_csv;
    std::string exclude_pid_csv;
    std::string filter_tid_csv;
    std::string exclude_tid_csv;
    std::string filter_function_csv;
    std::string exclude_function_csv;
    std::string filter_category_csv;
    std::string exclude_category_csv;

    std::uint64_t start_timestamp = 0;
    std::uint64_t end_timestamp = UINT64_MAX;
    std::int64_t min_size = -1;
    std::int64_t max_size = -1;
    double sample_rate = 1.0;
    std::uint64_t sample_seed = 0;
    std::size_t max_events = 0;
    std::size_t channel_capacity = 4096;

    explicit ReplayArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(pipeline);
    }

   protected:
    void register_args() override {
        auto& p = parser();
        p.add_argument("inputs")
            .help(
                "Trace files (.pfw, .pfw.gz) or directories containing trace "
                "files")
            .nargs(argparse::nargs_pattern::at_least_one);

        p.add_argument("--no-timing")
            .help("Ignore original timing and execute as fast as possible")
            .flag();
        p.add_argument("--dry-run")
            .help("Parse and analyze traces without executing operations")
            .flag();
        p.add_argument("--dftracer-mode")
            .help(
                "Use DFTracer sleep-based replay (sleep for operation "
                "duration instead of doing actual I/O)")
            .flag();
        p.add_argument("--no-sleep")
            .help(
                "When used with --dftracer-mode, disable sleep calls for "
                "maximum speed")
            .flag();
        p.add_argument("-r", "--recursive")
            .help("Recursively search directories for trace files")
            .flag();

        p.add_argument("--use-call-tree")
            .help("Build and use call tree structure for hierarchical replay")
            .flag();
        p.add_argument("--hierarchical-replay")
            .help(
                "Replay operations respecting parent-child call hierarchy "
                "(requires --use-call-tree)")
            .flag();
        p.add_argument("--respect-call-hierarchy")
            .help(
                "Replay child nodes immediately after parent (requires "
                "--use-call-tree and --hierarchical-replay)")
            .flag();

        p.add_argument("--filter-pid")
            .help("Only replay events from specific PID(s) (comma-separated)")
            .default_value(std::string(""));
        p.add_argument("--exclude-pid")
            .help("Exclude events from specific PID(s) (comma-separated)")
            .default_value(std::string(""));
        p.add_argument("--filter-tid")
            .help("Only replay events from specific TID(s) (comma-separated)")
            .default_value(std::string(""));
        p.add_argument("--exclude-tid")
            .help("Exclude events from specific TID(s) (comma-separated)")
            .default_value(std::string(""));
        p.add_argument("--filter-function")
            .help(
                "Only replay specific function(s) (comma-separated, e.g., "
                "'read,write,open')")
            .default_value(std::string(""));
        p.add_argument("--exclude-function")
            .help("Exclude specific function(s) (comma-separated)")
            .default_value(std::string(""));
        p.add_argument("--filter-category")
            .help(
                "Only replay specific category/categories (comma-separated, "
                "e.g., 'POSIX,storage')")
            .default_value(std::string(""));
        p.add_argument("--exclude-category")
            .help("Exclude specific category/categories (comma-separated)")
            .default_value(std::string(""));

        p.add_argument("--start-timestamp")
            .help("Only replay events after this timestamp (microseconds)")
            .default_value(std::uint64_t(0))
            .scan<'u', std::uint64_t>();
        p.add_argument("--end-timestamp")
            .help("Only replay events before this timestamp (microseconds)")
            .default_value(UINT64_MAX)
            .scan<'u', std::uint64_t>();
        p.add_argument("--min-size")
            .help("Only replay operations with size >= this value (bytes)")
            .default_value(std::int64_t(-1))
            .scan<'i', std::int64_t>();
        p.add_argument("--max-size")
            .help("Only replay operations with size <= this value (bytes)")
            .default_value(std::int64_t(-1))
            .scan<'i', std::int64_t>();

        p.add_argument("--sample-rate")
            .help("Sample rate for replay (0.0-1.0, 1.0=all events, 0.1=10%)")
            .default_value(1.0)
            .scan<'g', double>();
        p.add_argument("--sample-seed")
            .help("Random seed for sampling (for reproducibility)")
            .default_value(std::uint64_t(0))
            .scan<'u', std::uint64_t>();
        p.add_argument("--max-events")
            .help("Maximum number of events to replay (0=unlimited)")
            .default_value(std::size_t(0))
            .scan<'u', std::size_t>();
        p.add_argument("--channel-capacity")
            .help(
                "Bounded Channel<Trace> capacity between read/parse producer "
                "and dispatch consumer (default 4096)")
            .default_value(std::size_t(4096))
            .scan<'u', std::size_t>();
    }

    void post_parse() override {
        auto& p = parser();
        inputs = p.get<std::vector<std::string>>("inputs");
        no_timing = p.get<bool>("--no-timing");
        dry_run = p.get<bool>("--dry-run");
        dftracer_mode = p.get<bool>("--dftracer-mode");
        no_sleep = p.get<bool>("--no-sleep");
        recursive = p.get<bool>("--recursive");
        use_call_tree = p.get<bool>("--use-call-tree");
        hierarchical_replay = p.get<bool>("--hierarchical-replay");
        respect_call_hierarchy = p.get<bool>("--respect-call-hierarchy");

        filter_pid_csv = p.get<std::string>("--filter-pid");
        exclude_pid_csv = p.get<std::string>("--exclude-pid");
        filter_tid_csv = p.get<std::string>("--filter-tid");
        exclude_tid_csv = p.get<std::string>("--exclude-tid");
        filter_function_csv = p.get<std::string>("--filter-function");
        exclude_function_csv = p.get<std::string>("--exclude-function");
        filter_category_csv = p.get<std::string>("--filter-category");
        exclude_category_csv = p.get<std::string>("--exclude-category");

        start_timestamp = p.get<std::uint64_t>("--start-timestamp");
        end_timestamp = p.get<std::uint64_t>("--end-timestamp");
        min_size = p.get<std::int64_t>("--min-size");
        max_size = p.get<std::int64_t>("--max-size");
        sample_rate = p.get<double>("--sample-rate");
        sample_seed = p.get<std::uint64_t>("--sample-seed");
        max_events = p.get<std::size_t>("--max-events");
        channel_capacity = p.get<std::size_t>("--channel-capacity");
    }

    bool validate() override {
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
        return true;
    }
};

std::unordered_set<std::uint32_t> parse_csv_uint32(const std::string& csv) {
    std::unordered_set<std::uint32_t> out;
    for (const auto& token : cli::split_csv(csv)) {
        try {
            out.insert(static_cast<std::uint32_t>(std::stoul(token)));
        } catch (...) {
        }
    }
    return out;
}

std::unordered_set<std::string> parse_csv_string(const std::string& csv) {
    std::unordered_set<std::string> out;
    for (const auto& token : cli::split_csv(csv)) out.insert(token);
    return out;
}

struct RunCtx {
    const ReplayArgParse* cli = nullptr;
    int mpi_rank = 0;
    int mpi_size = 1;
    bool is_root = true;

    ReplayConfig config;
    std::vector<std::string> trace_files;
    ReplayResult result;
    int exit_code = 0;
    bool failed = false;

    double scan_ms = 0;
    double execute_ms = 0;
};

coro::CoroTask<void> task_scan(RunCtx* ctx, CoroScope& scope) {
    const auto t0 = std::chrono::steady_clock::now();

    ctx->trace_files = co_await cli::collect_input_trace_files(
        scope, ctx->cli->inputs, ctx->cli->recursive,
        [](const std::string& in) {
            DFTRACER_UTILS_LOG_ERROR("Input not found or not accessible: %s",
                                     in.c_str());
        });

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
                             const std::unordered_set<std::uint32_t>& s) {
        if (s.empty()) return;
        std::printf("  %s:", label);
        for (auto v : s) std::printf(" %u", v);
        std::printf("\n");
    };
    auto print_str_set = [](const char* label,
                            const std::unordered_set<std::string>& s) {
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

coro::CoroTask<void> task_execute(RunCtx* ctx, CoroScope* scope) {
    if (ctx->failed) co_return;

    const auto t0 = std::chrono::steady_clock::now();

    if (ctx->is_root) {
        print_configuration(*ctx);
        print_filters(*ctx);
        std::printf("\n=== Starting Replay ===\n");
    }

    ReplayEngine engine(ctx->config);

    if (ctx->config.use_call_tree) {
        // Call tree path stays sync — replay_with_call_tree builds the
        // full tree in memory first, so there's nothing to hide behind a
        // channel. We just call it from inside this task.
        if (ctx->cli->inputs.size() != 1 ||
            !fs::is_directory(ctx->cli->inputs[0])) {
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
        co_await engine.run_pipelined(*scope, ctx->trace_files, ctx->result,
                                      ctx->cli->channel_capacity);
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
    dftracer::utils::logger::init();

    argparse::ArgumentParser program("dftracer_replay",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "DFTracer replay utility - replays I/O operations from DFTracer "
        "trace files (.pfw, .pfw.gz)");

    ReplayArgParse cli(program);
    if (!cli::setup_and_parse(cli, argc, argv)) return 1;

    RunCtx ctx;
    ctx.cli = &cli;

#ifdef DFTRACER_UTILS_MPI_ENABLED
    ctx.mpi_rank = mpi::MPIUtils::instance().get_rank();
    ctx.mpi_size = mpi::MPIUtils::instance().get_world_size();
    ctx.is_root = mpi::MPIUtils::instance().is_root();
#endif

    // Mirror argparse output into ReplayConfig.
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

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Replay", cli.pipeline);
    Pipeline pipeline(pipeline_config);

    RunCtx* ctx_ptr = &ctx;
    auto scan = make_task(
        [ctx_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_scan(ctx_ptr, scope);
        },
        "scan");
    auto execute = make_task(
        [ctx_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_execute(ctx_ptr, &scope);
        },
        "execute");
    execute->depends_on(scan);

    pipeline.set_source(scan);
    pipeline.set_destination(execute);
    pipeline.execute();

    if (ctx.is_root) {
        DFTRACER_UTILS_LOG_DEBUG("[done] scan=%.1fms execute=%.1fms",
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
