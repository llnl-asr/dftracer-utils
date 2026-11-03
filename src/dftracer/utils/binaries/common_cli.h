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
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scope_exit.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/pipeline/pipeline_config.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
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
#include <vector>

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
            common::logger::level_name(common::logger::get_level()));
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
        return run(cli);
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

struct PipelineArgs : CliSchema {
    std::size_t executor_threads = 0;
    std::size_t io_threads = 0;
    bool time_profiling = false;
    bool eager = false;

    PipelineArgs() = default;

    void register_on(ArgParser& p) override {
        p.group("Pipeline");
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

    void apply(PipelineConfig& config) const {
        config.with_compute_threads(executor_threads);
        config.with_io_threads(io_threads);
        if (eager) config.with_eager();
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
    int idle_timeout = 300;
    int deadlock_timeout = 600;

    void register_on(ArgParser& p) override {
        p.group("Watchdog");
        p.flag(disable, {"--disable-watchdog"},
               "Disable watchdog for hang detection");
        p.seconds(global_timeout_, {"--watchdog-global-timeout"},
                  "Watchdog global timeout for pipeline execution (0 = no "
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
        p.seconds(idle_timeout_, {"--watchdog-idle-timeout"},
                  "Watchdog idle timeout (0 = use default). Accepts units",
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
        idle_timeout = whole_seconds(idle_timeout_);
        deadlock_timeout = whole_seconds(deadlock_timeout_);
    }

    void apply(PipelineConfig& config) const {
        config.with_watchdog(!disable)
            .with_global_timeout(std::chrono::seconds(global_timeout))
            .with_task_timeout(std::chrono::seconds(task_timeout))
            .with_watchdog_interval(std::chrono::seconds(interval))
            .with_warning_threshold(std::chrono::seconds(warning_threshold))
            .with_executor_idle_timeout(std::chrono::seconds(idle_timeout))
            .with_executor_deadlock_timeout(
                std::chrono::seconds(deadlock_timeout));
    }

   private:
    static int whole_seconds(double seconds) {
        return static_cast<int>(std::lround(seconds));
    }

    double global_timeout_ = 0;
    double task_timeout_ = 0;
    double interval_ = 1;
    double warning_threshold_ = 300;
    double idle_timeout_ = 300;
    double deadlock_timeout_ = 600;
};

// A CLI's run coroutine runs on main's one-thread coro::RunLoop, so work
// awaited outside a Pipeline is serial. A heavy step runs here instead, on the
// process runtime, and blocks until done.
template <typename Func>
void run_on_pool(const PipelineArgs& pipeline, const char* name, Func&& func) {
    RuntimeSettings settings;
    settings.threads = pipeline.executor_threads;
    settings.io_threads = pipeline.io_threads;
    if (pipeline.eager) settings.eager = true;
    configure_default_runtime(settings).block_on(
        coro::named(name, coro::with_scope(std::forward<Func>(func))));
}

inline PipelineConfig build_pipeline_config(const char* name,
                                            const PipelineArgs& pipeline) {
    auto config = PipelineConfig().with_name(name).with_watchdog(false);
    pipeline.apply(config);
    return config;
}

inline PipelineConfig build_pipeline_config(const char* name,
                                            const PipelineArgs& pipeline,
                                            const WatchdogArgs& watchdog) {
    auto config = PipelineConfig().with_name(name);
    pipeline.apply(config);
    watchdog.apply(config);
    return config;
}

using utilities::filesystem::is_trace_file;

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

// The trace files under `directory`, scanned recursively on a pool of its own
// (see scan_on_pool) with one I/O thread.
// keep std: unswept API
inline std::vector<utilities::filesystem::FileEntry> scan_trace_files_on_pool(
    const PipelineArgs& pipeline, dstd::StringView directory,
    bool populate_size = true) {
    return utilities::filesystem::scan_on_pool<
        utilities::filesystem::PatternDirectoryScannerUtility>(
        pipeline.executor_threads, 1,
        utilities::filesystem::PatternDirectoryScannerUtilityInput{
            dstd::String(directory),
            utilities::filesystem::trace_file_patterns(), true, populate_size});
}

// Parallel per-subdirectory scan of `directory` for trace files via
// ctx.spawn (context lets a recursive walk fan out per subdirectory); sizes
// are not populated. Returns the matched paths.
inline coro::CoroTask<dstd::Vector<dstd::String>> scan_directory_trace_files(
    coro::Scope& ctx, dstd::StringView directory, bool recursive) {
    utilities::filesystem::PatternDirectoryScannerUtility scanner;
    utilities::filesystem::PatternDirectoryScannerUtilityInput scan_input{
        directory, utilities::filesystem::trace_file_patterns(), recursive};
    auto matched = co_await scanner(ctx, scan_input);
    dstd::Vector<dstd::String> files;
    files.reserve(matched.size());
    for (const auto& entry : matched) {
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

// Run `body` as a single-node pipeline and return its int result. `body` is a
// template param (inlinable, no type erasure); it runs exactly once.
template <class Body>
int run_single_task(const char* name, const PipelineArgs& pipeline,
                    Body&& body) {
    Pipeline p(build_pipeline_config(name, pipeline));
    auto task = make_task(
        [body = std::forward<Body>(body)](CoroScope&) -> coro::CoroTask<int> {
            co_return co_await coro::with_scope(body);
        },
        name);
    p.set_source(task);
    p.set_destination(task);
    p.execute();
    return task->template get<int>();
}

// Split out so the run_single_task lambda below holds no coroutine locals,
// which ICEs GCC 12/13. Leaves the scan in `entries` so the caller resolves
// again without scanning.
inline coro::CoroTask<int> ensure_indexes_fresh_task(
    coro::Scope& ctx, dstd::StringView directory,
    dstd::Vector<dstd::StringView> files, dstd::StringView index_dir,
    // keep std: unswept API
    bool force_rebuild, std::vector<utilities::filesystem::FileEntry>* entries,
    dftracer::utils::index::schemas::dft::agg::AggregationConfig aggregation =
        dftracer::utils::index::schemas::dft::agg::
            standard_aggregation_config(),
    std::size_t checkpoint_size = 0) {
    if (!directory.empty()) {
        utilities::filesystem::PatternDirectoryScannerUtility scanner;
        *entries = co_await scanner(
            ctx,
            utilities::filesystem::PatternDirectoryScannerUtilityInput{
                directory, utilities::filesystem::trace_file_patterns(), true});
    } else {
        for (const auto& f : files) entries->emplace_back(dstd::StringView(f));
    }
    if (entries->empty()) co_return 0;
    dftracer::utils::index::IndexerOptions options;
    options.index_dir.assign(index_dir.data(), index_dir.size());
    options.bloom->required = false;
    options.aggregation = std::move(aggregation);
    if (checkpoint_size != 0) options.checkpoint_size = checkpoint_size;
    auto indexer = dftracer::utils::index::Indexer::open_scanned(
        *entries, std::move(options));
    if (force_rebuild)
        co_await indexer.rebuild(ctx);
    else
        co_await indexer.build(ctx);
    co_return 0;
}

// keep std: unswept API
inline std::vector<utilities::filesystem::FileEntry>
ensure_indexes_fresh_blocking(
    const char* name, const PipelineArgs& pipeline, dstd::StringView directory,
    dstd::Vector<dstd::StringView> files, dstd::StringView index_dir,
    bool force_rebuild = false,
    dftracer::utils::index::schemas::dft::agg::AggregationConfig aggregation =
        dftracer::utils::index::schemas::dft::agg::
            standard_aggregation_config(),
    std::size_t checkpoint_size = 0) {
    // keep std: unswept API
    std::vector<utilities::filesystem::FileEntry> entries;
    if (directory.empty() && files.empty()) return entries;
    run_single_task(
        name, pipeline, [&](coro::Scope& ctx) -> coro::CoroTask<int> {
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
