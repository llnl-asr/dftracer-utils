// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_BINARIES_COMMON_CLI_H
#define DFTRACER_UTILS_BINARIES_COMMON_CLI_H

#include <dftracer/utils/binaries/cli_args.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scope_exit.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/watchdog.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <type_traits>

namespace dftracer::utils::binaries {

class ArgParse;

struct CliSchema {
    virtual ~CliSchema() = default;
    virtual void register_on(ArgParser& p) = 0;
    virtual void after_parse() {}
    virtual bool validate() { return true; }
};

// Register/apply the shared --log-level flag on any parser. A CLI flag
// overrides the DFTRACER_UTILS_LOG_LEVEL environment variable.
inline void add_log_level_arg(ArgParser& p, dstd::StringView& name) {
    p.option(name, {"--log-level"},
             "Logging verbosity: trace, debug, info, warn, error, off");
}

inline void apply_log_level_arg(dstd::StringView name) {
    if (name.empty()) return;
    if (auto level = common::logger::level_from_name(name)) {
        common::logger::set_level(*level);
    } else {
        DFTRACER_UTILS_LOG_WARN(
            "Unknown --log-level '%.*s'; keeping '%s'",
            static_cast<int>(name.size()), name.data(),
            common::logger::level_name(common::logger::level()));
    }
}

class ArgParse {
   public:
    explicit ArgParse(ArgParser& parser) : parser_(parser) {}
    virtual ~ArgParse() = default;

    ArgParse(const ArgParse&) = delete;
    ArgParse& operator=(const ArgParse&) = delete;

    void setup() {
        for (auto* s : schemas_) s->register_on(parser_);
        add_log_level_arg(parser_, log_level_);
        register_args();
    }

    // Returns the process exit code when the run ends here (help, version,
    // a parse error or a failed check), nothing when the tool should run.
    dstd::Optional<int> parse(int argc, char** argv) {
        const ParseOutcome outcome = parser_.parse(argc, argv);
        if (outcome.exit_code) {
            if (!outcome.out.empty())
                std::fwrite(outcome.out.data(), 1, outcome.out.size(), stdout);
            if (!outcome.err.empty()) {
                DFTRACER_UTILS_LOG_ERROR("Error: %s", outcome.err.c_str());
                std::fprintf(stderr, "%s\n", parser_.help().c_str());
            }
            return outcome.exit_code;
        }
        try {
            apply_log_level_arg(log_level_);
            for (auto* s : schemas_) s->after_parse();
            post_parse();
        } catch (const std::exception& err) {
            DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
            std::fprintf(stderr, "%s\n", parser_.help().c_str());
            return 1;
        }
        for (auto* s : schemas_) {
            if (!s->validate()) return 1;
        }
        if (!validate()) return 1;
        return dstd::nullopt;
    }

    template <typename... Schemas>
    void schema(Schemas&... args) {
        (schemas_.push_back(&args), ...);
    }

   protected:
    virtual void register_args() {}
    virtual void post_parse() {}
    virtual bool validate() { return true; }

    ArgParser& parser() { return parser_; }
    const ArgParser& parser() const { return parser_; }

   private:
    ArgParser& parser_;
    dstd::Vector<CliSchema*> schemas_;
    dstd::StringView log_level_;
};

// Register schemas/args then parse argv. Returns the exit code when the run
// ends in the parse (help already printed), nothing otherwise.
template <class Cli>
dstd::Optional<int> setup_and_parse(Cli& cli, int argc, char** argv) {
    cli.setup();
    return cli.parse(argc, argv);
}

// Shared CLI main prologue: logger init, parser construction with the package
// version, description, ArgParse (CliT) setup+parse (returns its exit code),
// then `run(cli)`. `run` is a template param so it inlines with no type
// erasure; it receives the parsed CliT and returns the process exit code.
template <class CliT, class RunFn>
int cli_main(int argc, char** argv, const char* name, const char* description,
             RunFn&& run) {
    // Run the shutdown hooks at cli_main scope, while the runtime the store
    // writers wait on still runs.
    auto shutdown_guard =
        dftracer::utils::dstd::defer([] { dftracer::utils::dstd::shutdown(); });
    dftracer::utils::common::logger::init();
    if (auto configured = dftracer::utils::common::configure_memory_from_env();
        !configured) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s",
                                 configured.error().format().c_str());
        return 1;
    }
    // Before the parse, so the input paths it checks are stat'ed once.
    dftracer::utils::common::InputStatScope input_stats;
    ArgParser program(name, DFTRACER_UTILS_PACKAGE_VERSION, description);
    CliT cli(program);
    if (auto code = setup_and_parse(cli, argc, argv)) return *code;
    try {
        auto outcome = run(cli);
        if constexpr (std::is_same_v<decltype(outcome), int>) {
            return outcome;
        } else {
            if (!outcome) {
                DFTRACER_UTILS_LOG_ERROR("Error: %s",
                                         outcome.error().format().c_str());
                return 1;
            }
            return *outcome;
        }
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        return 1;
    }
}

enum class DirMode { DefaultDot, DefaultEmpty, Required };

/// The positional paths of a tool, files and folders mixed. At most one
/// folder is taken, with any number of files. The views point into argv.
// A path view here is a whole argv string, so data() ends in a NUL.
struct InputArgs : CliSchema {
    DirMode mode = DirMode::DefaultDot;
    dstd::StringView help = "Trace files or a folder of trace files";
    dstd::Vector<dstd::StringView> paths;
    dstd::StringView directory;
    dstd::Vector<dstd::StringView> files;
    bool folder_only = false;

    InputArgs() = default;
    explicit InputArgs(DirMode m) : mode(m) {}
    InputArgs(DirMode m, dstd::StringView h) : mode(m), help(h) {}

    void register_on(ArgParser& p) override {
        auto& spec = p.positional(paths, "PATH", help);
        if (mode == DirMode::Required) spec.at_least_one();
    }

    void after_parse() override {
        if (mode == DirMode::DefaultDot && paths.empty()) {
            directory = ".";
            return;
        }
        for (const auto path : paths) {
            dftracer::utils::common::InputFileStat st;
            if (!dftracer::utils::common::stat_input(
                    dstd::CStringView(path.data()), st)) {
                if (missing_.empty()) missing_ = path;
                continue;
            }
            if (st.is_directory) {
                if (!directory.empty()) {
                    folder_error_ = true;
                    continue;
                }
                directory = path;
            } else {
                files.push_back(path);
            }
        }
    }

    bool validate() override {
        if (folder_only && !files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "This tool takes a folder, not files: %.*s",
                static_cast<int>(files.front().size()), files.front().data());
            return false;
        }
        if (folder_error_) {
            DFTRACER_UTILS_LOG_ERROR("%s", "Give at most one folder");
            return false;
        }
        if (!missing_.empty()) {
            DFTRACER_UTILS_LOG_ERROR("Input does not exist: %.*s",
                                     static_cast<int>(missing_.size()),
                                     missing_.data());
            return false;
        }
        return true;
    }

   private:
    bool folder_error_ = false;
    dstd::StringView missing_;
};

/// --memory-budget, shared by every tool that bounds its memory. 0 means auto,
/// a third of the memory available to the process.
struct MemoryArgs : CliSchema {
    std::uint64_t memory_budget = 0;
    dstd::StringView help =
        "Most memory the tool holds, with units (e.g. 64GB). Work beyond it "
        "spills to DFTRACER_UTILS_SPILL_DIR. 0 means a third of the "
        "available memory";

    void register_on(ArgParser& p) override {
        p.group("Memory");
        p.bytes(memory_budget, {"--memory-budget"}, help, "0");
    }
};

struct RuntimeArgs : CliSchema {
    std::size_t executor_threads = 0;
    std::size_t io_threads = 0;
    bool time_profiling = false;
    bool eager = false;

    RuntimeArgs() = default;

    void register_on(ArgParser& p) override {
        p.group("Runtime");
        executor_threads = common::hardware_concurrency();
        io_threads = common::hardware_concurrency();
        p.option(executor_threads, {"--executor-threads"},
                 "Number of worker threads for parallel processing "
                 "(default: number of CPU cores)");
        p.option(io_threads, {"--io-threads"},
                 "Number of I/O threads "
                 "(default: number of CPU cores)");
        p.flag(time_profiling, {"--time-profiling"},
               "Print stage timing breakdown to stderr");
        p.flag(eager, {"--eager-thread-pools"},
               "Start the full worker pool up front instead of growing it on "
               "demand (lower first-batch latency, holds all threads)");
    }

    bool validate() override {
        if (executor_threads == 0) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "--executor-threads must be greater than 0");
            return false;
        }
        return true;
    }

    void apply(RuntimeSettings& settings) const {
        settings.threads = executor_threads;
        settings.io_threads = io_threads;
        if (eager) settings.eager = true;
        settings.deadlock_timeout =
            std::chrono::seconds(common::constants::executor_settings::
                                     TOOL_DEADLOCK_TIMEOUT_SECONDS);
    }
};

struct IndexingArgs : CliSchema {
    dstd::StringView index_dir;
    std::size_t checkpoint_size = 0;
    bool force = false;

    dstd::StringView index_dir_help = "Directory for .dftindex stores";
    dstd::StringView force_help = "Force index recreation";
    dstd::String checkpoint_default =
        dstd::to_string(common::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    dstd::String checkpoint_help =
        "Checkpoint size for gzip indexing (default: " + checkpoint_default +
        "). Accepts units, e.g. 512KB, 4MB";
    dstd::StringView group = "Indexing";
    bool with_index_dir = true;
    bool with_force = true;
    bool with_checkpoint = true;

    IndexingArgs() = default;
    explicit IndexingArgs(bool f) : with_force(f) {}

    void register_on(ArgParser& p) override {
        p.group(group);
        if (with_index_dir) {
            p.option(index_dir, {"--index-dir"}, index_dir_help);
        }
        if (with_checkpoint) {
            p.bytes(checkpoint_size, {"--checkpoint-size"}, checkpoint_help,
                    checkpoint_default);
        }
        if (with_force) {
            p.flag(force, {"-f", "--force"}, force_help);
        }
    }
};

struct DuqlArgs : CliSchema {
    dstd::StringView duql;
    dstd::Vector<dstd::StringView> macro_paths;
    dstd::StringView help =
        "duql filter (e.g., 'cat == \"POSIX\" and dur > 1000')";

    DuqlArgs() = default;
    explicit DuqlArgs(dstd::StringView h) : help(h) {}

    void register_on(ArgParser& p) override {
        p.group("duql");
        p.option(duql, {"-q", "--query"}, help);
        p.option(macro_paths, {"--duql-path"},
                 "A .duql macro file or a directory of them, read after "
                 "$DFTRACER_UTILS_DUQL_PATH (repeatable)")
            .append();
    }

    void after_parse() override {
        for (const auto& path : macro_paths)
            dftracer::utils::duql::load_macros(path);
    }
};

struct WatchdogArgs : CliSchema {
    bool disable = false;
    int global_timeout = 0;
    int task_timeout = 0;
    int interval = 1;
    int warning_threshold = 300;
    int deadlock_timeout = 600;

    void register_on(ArgParser& p) override {
        p.group("Watchdog");
        p.flag(disable, {"--disable-watchdog"},
               "Disable watchdog for hang detection");
        p.seconds(global_timeout_, {"--watchdog-global-timeout"},
                  "Watchdog global timeout for the run (0 = no "
                  "timeout). Accepts units, e.g. 30s, 5m",
                  "0");
        p.seconds(task_timeout_, {"--watchdog-task-timeout"},
                  "Watchdog default task timeout (0 = no timeout). Accepts "
                  "units, e.g. 30s, 5m",
                  "0");
        p.seconds(interval_, {"--watchdog-interval"},
                  "Watchdog check interval. Accepts units, e.g. 1s, 500ms",
                  "1");
        p.seconds(warning_threshold_, {"--watchdog-warning-threshold"},
                  "Watchdog long-running task warning threshold. Accepts "
                  "units, e.g. 300s, 5m",
                  "300");
        p.seconds(deadlock_timeout_, {"--watchdog-deadlock-timeout"},
                  "Watchdog deadlock timeout (0 = use default). Accepts units",
                  "600");
    }

    void after_parse() override {
        global_timeout = whole_seconds(global_timeout_);
        task_timeout = whole_seconds(task_timeout_);
        interval = whole_seconds(interval_);
        warning_threshold = whole_seconds(warning_threshold_);
        deadlock_timeout = whole_seconds(deadlock_timeout_);
    }

    void apply(RuntimeSettings& settings) const {
        if (deadlock_timeout > 0)
            settings.deadlock_timeout = std::chrono::seconds(deadlock_timeout);
    }

   private:
    static int whole_seconds(double seconds) {
        return static_cast<int>(std::lround(seconds));
    }

    double global_timeout_ = 0;
    double task_timeout_ = 0;
    double interval_ = 1;
    double warning_threshold_ = 300;
    double deadlock_timeout_ = 600;
};

// The value of a task run by run_on_pool. A task that ended with an error is
// raised, which cli_main reports as the error of the tool.
template <class R>
R take_pool_result(coro::TaskResult<R>&& result) {
    if constexpr (coro::detail::is_expected_v<R>) {
        return std::move(result);
    } else {
        if (!result) common::throw_error(result.error());
        if constexpr (!std::is_void_v<R>) return std::move(*result);
    }
}

// A CLI's run coroutine runs on main's one-thread coro::RunLoop, so work
// awaited outside the runtime is serial. A tool runs here instead, as one named
// task on the process runtime, and blocks until done. `body` takes a
// coro::Scope& and returns a coro::CoroTask.
template <class Body>
auto run_on_pool(const char* name, const RuntimeArgs& runtime, Body&& body) {
    RuntimeSettings settings;
    runtime.apply(settings);
    using R = typename std::invoke_result_t<std::decay_t<Body>&,
                                            coro::Scope&>::value_type;
    return take_pool_result<R>(configure_default_runtime(settings).block_on(
        coro::named(name, coro::with_scope(std::forward<Body>(body)))));
}

// As above, with a watchdog that stops the task and raises a
// dstd::Condition::Timeout error when a timeout passes.
template <class Body>
auto run_on_pool(const char* name, const RuntimeArgs& runtime,
                 const WatchdogArgs& watchdog, Body&& body) {
    using Result = typename std::invoke_result_t<std::decay_t<Body>&,
                                                 coro::Scope&>::value_type;
    RuntimeSettings settings;
    runtime.apply(settings);
    watchdog.apply(settings);
    Runtime& rt = configure_default_runtime(settings);
    if (watchdog.disable) {
        return take_pool_result<Result>(rt.block_on(
            coro::named(name, coro::with_scope(std::forward<Body>(body)))));
    }

    coro::StopSource source;
    dstd::String reason;
    Watchdog dog(std::chrono::seconds(watchdog.interval),
                 std::chrono::seconds(watchdog.global_timeout),
                 std::chrono::seconds(watchdog.task_timeout),
                 std::chrono::seconds(watchdog.warning_threshold));
    dog.set_executor(rt.executor());
    dog.set_timeout_callback([&](dstd::StringView why) {
        reason.assign(why.data(), why.size());
        DFTRACER_UTILS_LOG_ERROR("Watchdog timeout: %s", reason.c_str());
        source.request_stop(rt.executor());
    });
    dog.set_warning_callback(
        [](dstd::StringView task_name, std::int64_t elapsed_ms) {
            DFTRACER_UTILS_LOG_WARN("Long-running task: %.*s (%lld ms)",
                                    static_cast<int>(task_name.size()),
                                    task_name.data(),
                                    static_cast<long long>(elapsed_ms));
        });
    dog.mark_execution_start();
    dog.start();
    dog.register_task_start(0, name);

    try {
        auto task = coro::named(
            name,
            coro::with_stop_token(coro::with_scope(std::forward<Body>(body)),
                                  source.token()));
        auto outcome = rt.block_on(std::move(task));
        dog.stop();
        // A timeout that fires after the task ended does not undo its result.
        if (!outcome && !reason.empty() &&
            outcome.error().is(dstd::Condition::Cancelled)) {
            throw common::DFTUtilsException(
                dstd::Error{dstd::Condition::Timeout, "[TIMEOUT] " + reason});
        }
        return take_pool_result<Result>(std::move(outcome));
    } catch (...) {
        // The watchdog thread is joined before `reason` is read.
        dog.stop();
        if (!reason.empty()) {
            throw common::DFTUtilsException(
                dstd::Error{dstd::Condition::Timeout, "[TIMEOUT] " + reason});
        }
        throw;
    }
}

// Split a comma-separated list into views of `text`, dropping empty fields.
inline dstd::Vector<dstd::StringView> split_csv(dstd::StringView text) {
    dstd::Vector<dstd::StringView> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        if (!item.empty()) out.push_back(item);
        if (comma == dstd::StringView::npos) break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

// How many input files a tool may hold open at once. Raises the soft open-file
// limit to the hard limit and keeps half of it for the tool's other files.
inline std::size_t open_file_budget() {
    struct rlimit lim{};
    if (::getrlimit(RLIMIT_NOFILE, &lim) != 0) return 0;
    if (lim.rlim_cur < lim.rlim_max) {
        lim.rlim_cur = lim.rlim_max;
        if (::setrlimit(RLIMIT_NOFILE, &lim) != 0)
            ::getrlimit(RLIMIT_NOFILE, &lim);
    }
    constexpr rlim_t RESERVED = 256;
    if (lim.rlim_cur == RLIM_INFINITY) return static_cast<std::size_t>(1) << 20;
    return lim.rlim_cur > RESERVED
               ? static_cast<std::size_t>((lim.rlim_cur - RESERVED) / 2)
               : 0;
}

// Warn when an aggregated workload of `required_bytes` will not fit in one
// process (peak is ~PEAK_MEMORY_FACTOR x that); the message says how much
// memory or how many nodes it needs. `available_bytes = 0` detects it
// (cgroup-aware). The Python/dfanalyzer path emits the same message via
// memory_budget_advice.
inline void warn_if_memory_tight(std::size_t required_bytes,
                                 std::size_t available_bytes = 0) {
    const auto advice = dftracer::utils::common::memory_budget_advice(
        required_bytes, available_bytes);
    if (!advice.fits) {
        DFTRACER_UTILS_LOG_WARN(
            "%s", dftracer::utils::common::format_memory_budget_warning(advice)
                      .c_str());
    }
}

// The absolute, normalized form of `path`. Throws when the current directory
// cannot be read.
inline dstd::Path absolute_path(dstd::StringView path) {
    return common::unwrap(dstd::fs::absolute(dstd::PathView(path)));
}

// The input traces below a folder, and no folder of index data, split output
// or schemas. An unreadable folder is skipped in a recursive scan.
inline runtime::fs::FindOptions trace_find_options(bool recursive,
                                                   bool with_stat) {
    runtime::fs::FindOptions options;
    options.patterns =
        runtime::fs::names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
    options.skip_dirs =
        runtime::fs::names_of(common::constants::indexer::SCAN_SKIP_DIRS);
    options.recursive = recursive;
    options.with_stat = with_stat;
    options.skip_unreadable = recursive;
    return options;
}

// The trace files under `directory`, scanned recursively on a pool of its own
// (see runtime::fs::find_all_on_pool). Stats run on I/O threads, so it needs
// as many of them as workers.
inline dstd::Vector<runtime::fs::FoundFile> scan_trace_files_on_pool(
    const RuntimeArgs& runtime, dstd::StringView directory,
    bool with_stat = true) {
    auto found = runtime::fs::find_all_on_pool(
        runtime.executor_threads, runtime.executor_threads,
        dstd::PathView(directory), trace_find_options(true, with_stat));
    if (!found) throw common::DFTUtilsException(found.error());
    return std::move(*found);
}

// Parallel per-subdirectory scan of `directory` for trace files over the
// scope's executor; sizes are not populated. Returns the matched paths.
inline coro::CoroTask<dstd::Vector<dstd::String>> scan_directory_trace_files(
    coro::Scope& ctx, dstd::StringView directory, bool recursive) {
    auto options = trace_find_options(recursive, /*with_stat=*/false);
    auto found = co_await runtime::fs::find_all(ctx, dstd::PathView(directory),
                                                std::move(options));
    if (!found) throw common::DFTUtilsException(found.error());
    dstd::Vector<dstd::String> files;
    files.reserve(found->size());
    for (const auto& entry : *found) {
        files.emplace_back(entry.path.str());
    }
    co_return files;
}

// Enumerate trace files from input paths (each a file or directory).
// Directories are scanned via the parallel scanner above; results are sorted
// for deterministic output. `on_missing(path)` runs for inputs that are
// neither a regular file nor a directory (template param so it inlines).
template <class OnMissing>
coro::CoroTask<dstd::Vector<dstd::String>> collect_input_trace_files(
    coro::Scope& ctx, const dstd::Vector<dstd::StringView>& inputs,
    bool recursive, OnMissing&& on_missing) {
    dstd::Vector<dstd::String> out;
    for (const auto in : inputs) {
        if (dftracer::utils::common::input_is_directory(
                dstd::CStringView(in.data()))) {
            auto scanned =
                co_await scan_directory_trace_files(ctx, in, recursive);
            out.insert(out.end(), std::make_move_iterator(scanned.begin()),
                       std::make_move_iterator(scanned.end()));
        } else if (dftracer::utils::common::input_is_regular_file(
                       dstd::CStringView(in.data()))) {
            out.emplace_back(in);
        } else {
            on_missing(in);
        }
    }
    std::sort(out.begin(), out.end());
    co_return out;
}

inline coro::CoroTask<dstd::Vector<dstd::String>> collect_input_trace_files(
    coro::Scope& ctx, const dstd::Vector<dstd::StringView>& inputs,
    bool recursive) {
    co_return co_await collect_input_trace_files(ctx, inputs, recursive,
                                                 [](dstd::StringView) {});
}

// Split out so the lambda that calls it holds no coroutine locals, which ICEs
// GCC 12/13. Leaves the scan in `entries` so the caller resolves
// again without scanning.
inline coro::CoroTask<int> ensure_indexes_fresh_task(
    coro::Scope& ctx, dstd::StringView directory,
    dstd::Vector<dstd::StringView> files, dstd::StringView index_dir,
    bool force_rebuild, dstd::Vector<runtime::fs::FoundFile>* entries,
    dftracer::utils::index::schemas::dft::agg::AggregationConfig aggregation =
        dftracer::utils::index::schemas::dft::agg::
            standard_aggregation_config(),
    std::size_t checkpoint_size = 0) {
    if (!directory.empty()) {
        auto options = trace_find_options(true, /*with_stat=*/true);
        auto found = co_await runtime::fs::find_all(
            ctx, dstd::PathView(directory), std::move(options));
        if (!found) throw common::DFTUtilsException(found.error());
        *entries = std::move(*found);
    } else {
        dstd::Vector<dstd::Path> paths;
        paths.reserve(files.size());
        for (const auto& f : files) paths.emplace_back(f);
        auto stat_task = runtime::fs::stat_many(
            dstd::Span<const dstd::Path>(paths.data(), paths.size()));
        auto stats = co_await std::move(stat_task);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            runtime::fs::FoundFile entry;
            entry.path = paths[i];
            if (stats[i] && stats[i]->kind == dstd::fs::FileKind::File) {
                entry.size = stats[i]->size;
                entry.mtime_ns = stats[i]->mtime_ns;
            }
            entries->push_back(std::move(entry));
        }
    }
    if (entries->empty()) co_return 0;
    dftracer::utils::index::IndexerOptions options;
    options.index_dir.assign(index_dir.data(), index_dir.size());
    options.bloom->required = false;
    options.aggregation = std::move(aggregation);
    if (checkpoint_size != 0) options.checkpoint_size = checkpoint_size;
    auto indexer = dftracer::utils::index::detail::open_scanned(
        *entries, std::move(options));
    if (force_rebuild)
        co_await indexer.rebuild(ctx);
    else
        co_await indexer.build(ctx);
    co_return 0;
}

inline dstd::Vector<runtime::fs::FoundFile> ensure_indexes_fresh_blocking(
    const char* name, const RuntimeArgs& runtime, dstd::StringView directory,
    dstd::Vector<dstd::StringView> files, dstd::StringView index_dir,
    bool force_rebuild = false,
    dftracer::utils::index::schemas::dft::agg::AggregationConfig aggregation =
        dftracer::utils::index::schemas::dft::agg::
            standard_aggregation_config(),
    std::size_t checkpoint_size = 0) {
    dstd::Vector<runtime::fs::FoundFile> entries;
    if (directory.empty() && files.empty()) return entries;
    run_on_pool(name, runtime, [&](coro::Scope& ctx) -> coro::CoroTask<int> {
        co_return co_await ensure_indexes_fresh_task(
            ctx, directory, std::move(files), index_dir, force_rebuild,
            &entries, std::move(aggregation), checkpoint_size);
    });
    return entries;
}

// Channel-backed fan-out: a single bounded producer feeds `items` (moved in)
// to `threads` consumers, each running `co_await worker(item)`. `worker` is a
// template param so it inlines on the work path (never std::function). Spawns
// onto `scope`; the caller's enclosing scope waits for completion.
template <class Range, class Worker>
void parallel_for_each(coro::Scope& scope, Range items, std::size_t threads,
                       Worker worker) {
    using Item = std::decay_t<decltype(*std::begin(items))>;
    auto chan = coro::make_channel<Item>(threads * 2);
    scope.spawn([ch = chan->producer(),
                 items = std::move(items)]() mutable -> coro::CoroTask<void> {
        auto guard = ch.guard();
        for (const auto& item : items) {
            const auto send_ok = co_await ch.send(item);
            if (!send_ok) co_return;
        }
        co_return;
    });
    for (std::size_t w = 0; w < threads; ++w) {
        scope.spawn(
            [ch = chan->consumer(), worker]() mutable -> coro::CoroTask<void> {
                for (;;) {
                    auto item = co_await ch.receive();
                    if (!item) break;
                    co_await worker(*item);
                }
                co_return;
            });
    }
}

}  // namespace dftracer::utils::binaries

#endif  // DFTRACER_UTILS_BINARIES_COMMON_CLI_H
