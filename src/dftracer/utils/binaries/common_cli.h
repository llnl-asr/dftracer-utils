#ifndef DFTRACER_UTILS_BINARIES_COMMON_CLI_H
#define DFTRACER_UTILS_BINARIES_COMMON_CLI_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/common/str_format.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace dftracer::utils::cli {

class ArgParse;

struct CliSchema {
    virtual ~CliSchema() = default;
    virtual void register_on(argparse::ArgumentParser& p) = 0;
    virtual void parse_from(const argparse::ArgumentParser& p) = 0;
    virtual bool validate() { return true; }
};

// Register/apply the shared --log-level flag on any parser (used by the
// ArgParse base and the few CLIs that parse argparse directly). A CLI flag
// overrides the DFTRACER_UTILS_LOG_LEVEL environment variable.
inline void add_log_level_arg(argparse::ArgumentParser& p) {
    p.add_argument("--log-level")
        .help("Logging verbosity: trace, debug, info, warn, error, off")
        .default_value(std::string(""));
}

inline void apply_log_level_arg(const argparse::ArgumentParser& p) {
    const auto name = p.get<std::string>("--log-level");
    if (name.empty()) return;
    if (auto level = logger::level_from_name(name)) {
        logger::set_level(*level);
    } else {
        DFTRACER_UTILS_LOG_WARN("Unknown --log-level '%s'; keeping '%s'",
                                name.c_str(),
                                logger::level_name(logger::get_level()));
    }
}

class ArgParse {
   public:
    explicit ArgParse(argparse::ArgumentParser& parser) : parser_(parser) {}
    virtual ~ArgParse() = default;

    ArgParse(const ArgParse&) = delete;
    ArgParse& operator=(const ArgParse&) = delete;

    void setup() {
        for (auto* s : schemas_) s->register_on(parser_);
        add_log_level_arg(parser_);
        register_args();
    }

    bool parse(int argc, char** argv) {
        try {
            parser_.parse_args(argc, argv);
            apply_log_level_arg(parser_);
            for (auto* s : schemas_) s->parse_from(parser_);
            post_parse();
        } catch (const std::exception& err) {
            DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
            std::fprintf(stderr, "%s\n", parser_.help().str().c_str());
            return false;
        }
        for (auto* s : schemas_) {
            if (!s->validate()) return false;
        }
        return validate();
    }

    template <typename... Schemas>
    void schema(Schemas&... args) {
        (schemas_.push_back(&args), ...);
    }

   protected:
    virtual void register_args() {}
    virtual void post_parse() {}
    virtual bool validate() { return true; }

    argparse::ArgumentParser& parser() { return parser_; }
    const argparse::ArgumentParser& parser() const { return parser_; }

   private:
    argparse::ArgumentParser& parser_;
    std::vector<CliSchema*> schemas_;
};

// Register schemas/args then parse argv; returns false on parse error (help
// is already printed). Collapses the two-line prologue every CLI main repeats.
template <class Cli>
bool setup_and_parse(Cli& cli, int argc, char** argv) {
    cli.setup();
    return cli.parse(argc, argv);
}

// Shared CLI main prologue: logger init, parser construction with the package
// version, description, ArgParse (CliT) setup+parse (returns 1 on failure),
// then `run(cli)`. `run` is a template param so it inlines with no type
// erasure; it receives the parsed CliT and returns the process exit code.
template <class CliT, class RunFn>
int cli_main(int argc, char** argv, const char* name, const char* description,
             RunFn&& run) {
    // Close process-lifetime RocksDB caches (e.g. the aggregation tier cache,
    // which holds read-only agg DBs open for reuse) at true process exit, while
    // RocksDB's globals are still alive - running it here at cli_main scope,
    // not via atexit, avoids the static-destruction-order races that abandon
    // the handles and leak them.
    struct RocksDbExitGuard {
        ~RocksDbExitGuard() {
            dftracer::utils::rocksdb::mark_process_exiting_for_rocksdb();
        }
    } rocksdb_exit_guard;
    dftracer::utils::logger::init();
    argparse::ArgumentParser program(name, DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(description);
    CliT cli(program);
    if (!setup_and_parse(cli, argc, argv)) return 1;
    return run(cli);
}

// Human-readable byte count, e.g. "1.5 MB" or "3.0 MB/s"; see
// dftracer::utils::human_bytes.
inline std::string human_bytes(double value, const char* per_suffix = "",
                               int precision = 1) {
    return dftracer::utils::human_bytes(value, per_suffix, precision);
}

// Read a string-valued argument as a byte size, accepting human-readable units
// (e.g. "64MB", "1.5GiB") as well as a bare byte count. Throws
// std::runtime_error on malformed input so ArgParse::parse reports it like any
// other parse error.
inline std::size_t get_bytes_arg(const argparse::ArgumentParser& p,
                                 const char* name) {
    const auto raw = p.get<std::string>(name);
    auto parsed = dftracer::utils::parse_bytes(raw);
    if (!parsed) {
        throw std::runtime_error(
            dftracer::utils::str_cat("Invalid byte size for ", name, ": '", raw,
                                     "' (use e.g. 65536, 64KB, 1.5GB)"));
    }
    return static_cast<std::size_t>(*parsed);
}

// As get_bytes_arg, but a bare number is measured in `native_bytes` units (e.g.
// 1024*1024 when the flag has historically taken MB); a unit-suffixed value is
// absolute. Preserves back-compat for MB-denominated flags.
inline std::size_t get_bytes_arg(const argparse::ArgumentParser& p,
                                 const char* name, std::uint64_t native_bytes) {
    const auto raw = p.get<std::string>(name);
    auto parsed = dftracer::utils::parse_bytes_as(raw, native_bytes);
    if (!parsed) {
        throw std::runtime_error(
            dftracer::utils::str_cat("Invalid byte size for ", name, ": '", raw,
                                     "' (use e.g. 4, 4MB, 512KB)"));
    }
    return static_cast<std::size_t>(*parsed);
}

// As get_bytes_arg, but a negative value passes through unchanged as a sentinel
// (e.g. -1 = "no limit"); non-negative values parse as byte sizes.
inline std::int64_t get_bytes_arg_signed(const argparse::ArgumentParser& p,
                                         const char* name) {
    const auto raw = p.get<std::string>(name);
    const auto trimmed = dftracer::utils::detail::trim_ws(raw);
    if (!trimmed.empty() && trimmed.front() == '-') {
        return static_cast<std::int64_t>(
            std::strtoll(raw.c_str(), nullptr, 10));
    }
    return static_cast<std::int64_t>(get_bytes_arg(p, name));
}

// Read a string-valued argument as a duration in seconds, accepting units
// (e.g. "30s", "5m", "1.5h") as well as a bare second count. Throws
// std::runtime_error on malformed input.
inline double get_seconds_arg(const argparse::ArgumentParser& p,
                              const char* name) {
    const auto raw = p.get<std::string>(name);
    auto parsed = dftracer::utils::parse_duration_seconds(raw);
    if (!parsed) {
        throw std::runtime_error(
            dftracer::utils::str_cat("Invalid duration for ", name, ": '", raw,
                                     "' (use e.g. 30, 30s, 5m, 1.5h)"));
    }
    return *parsed;
}

// Read a string-valued duration argument, returning it in the flag's native
// unit (`native_per_second`, e.g. 1e3 for ms, 1e6 for us). A bare number keeps
// that native unit; a unit-suffixed value ("5m") is converted. Throws on
// malformed input.
inline double get_duration_arg(const argparse::ArgumentParser& p,
                               const char* name, double native_per_second) {
    const auto raw = p.get<std::string>(name);
    auto parsed = dftracer::utils::parse_duration_as(raw, native_per_second);
    if (!parsed) {
        throw std::runtime_error(
            dftracer::utils::str_cat("Invalid duration for ", name, ": '", raw,
                                     "' (use e.g. 5000, 5s, 500ms, 1.5h)"));
    }
    return *parsed;
}

enum class DirMode { DEFAULT_DOT, DEFAULT_EMPTY, REQUIRED };

struct DirectoryArgs : CliSchema {
    DirMode mode = DirMode::DEFAULT_DOT;
    std::string help = "Directory containing trace files";
    std::string value;

    DirectoryArgs() = default;
    explicit DirectoryArgs(DirMode m) : mode(m) {}
    DirectoryArgs(DirMode m, std::string h) : mode(m), help(std::move(h)) {}

    void register_on(argparse::ArgumentParser& p) override {
        auto& arg = p.add_argument("-d", "--directory").help(help);
        switch (mode) {
            case DirMode::DEFAULT_DOT:
                arg.default_value<std::string>(".");
                break;
            case DirMode::DEFAULT_EMPTY:
                arg.default_value<std::string>("");
                break;
            case DirMode::REQUIRED:
                arg.required();
                break;
        }
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        value = p.get<std::string>("--directory");
    }

    bool validate() override {
        if (mode == DirMode::REQUIRED && value.empty()) {
            DFTRACER_UTILS_LOG_ERROR("%s", "--directory is required");
            return false;
        }
        if (!value.empty() && !fs::exists(value)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     value.c_str());
            return false;
        }
        return true;
    }
};

struct FilesArgs : CliSchema {
    std::string help = "Trace files (.pfw, .pfw.gz)";
    std::vector<std::string> value;

    FilesArgs() = default;
    explicit FilesArgs(std::string h) : help(std::move(h)) {}

    void register_on(argparse::ArgumentParser& p) override {
        p.add_argument("--files")
            .help(help)
            .nargs(argparse::nargs_pattern::any)
            .default_value<std::vector<std::string>>({});
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        value = p.get<std::vector<std::string>>("--files");
    }
};

struct PipelineArgs : CliSchema {
    std::size_t executor_threads = 0;
    std::size_t io_threads = 0;
    bool time_profiling = false;
    bool eager = false;

    PipelineArgs() = default;

    void register_on(argparse::ArgumentParser& p) override {
        p.add_group("Pipeline");
        p.add_argument("--executor-threads")
            .help(
                "Number of worker threads for parallel processing "
                "(default: number of CPU cores)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(hardware_concurrency()));
        p.add_argument("--io-threads")
            .help(
                "Number of I/O threads "
                "(default: number of CPU cores)")
            .scan<'d', std::size_t>()
            .default_value(hardware_concurrency());
        p.add_argument("--time-profiling")
            .help("Print stage timing breakdown to stderr")
            .flag();
        p.add_argument("--eager-thread-pools")
            .help(
                "Start the full worker pool up front instead of growing it on "
                "demand (lower first-batch latency, holds all threads)")
            .flag();
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        executor_threads = p.get<std::size_t>("--executor-threads");
        io_threads = p.get<std::size_t>("--io-threads");
        time_profiling = p.get<bool>("--time-profiling");
        eager = p.get<bool>("--eager-thread-pools");
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
    std::string index_dir;
    std::size_t checkpoint_size = 0;
    bool force = false;

    std::string index_dir_help = "Directory for .dftindex stores";
    std::string force_help = "Force index recreation";
    bool with_index_dir = true;
    bool with_force = true;

    IndexingArgs() = default;
    explicit IndexingArgs(bool f) : with_force(f) {}

    void register_on(argparse::ArgumentParser& p) override {
        p.add_group("Indexing");
        if (with_index_dir) {
            p.add_argument("--index-dir")
                .help(index_dir_help)
                .default_value<std::string>("");
        }
        p.add_argument("--checkpoint-size")
            .help("Checkpoint size for gzip indexing (default: " +
                  std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE) +
                  "). Accepts units, e.g. 512KB, 4MB")
            .default_value(
                std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE));
        if (with_force) {
            p.add_argument("-f", "--force").help(force_help).flag();
        }
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        if (with_index_dir) {
            index_dir = p.get<std::string>("--index-dir");
        }
        checkpoint_size = get_bytes_arg(p, "--checkpoint-size");
        if (with_force) {
            force = p.get<bool>("--force");
        }
    }
};

struct QueryArgs : CliSchema {
    std::string query;
    std::string help =
        "Query DSL filter (e.g., 'cat == \"POSIX\" and dur > 1000')";

    QueryArgs() = default;
    explicit QueryArgs(std::string h) : help(std::move(h)) {}

    void register_on(argparse::ArgumentParser& p) override {
        p.add_group("Query");
        p.add_argument("--query").help(help).default_value<std::string>("");
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        query = p.get<std::string>("--query");
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

    void register_on(argparse::ArgumentParser& p) override {
        p.add_group("Watchdog");
        p.add_argument("--disable-watchdog")
            .help("Disable watchdog for hang detection")
            .flag();
        p.add_argument("--watchdog-global-timeout")
            .help(
                "Watchdog global timeout for pipeline execution (0 = no "
                "timeout). Accepts units, e.g. 30s, 5m")
            .default_value(std::string("0"));
        p.add_argument("--watchdog-task-timeout")
            .help(
                "Watchdog default task timeout (0 = no timeout). Accepts "
                "units, e.g. 30s, 5m")
            .default_value(std::string("0"));
        p.add_argument("--watchdog-interval")
            .help("Watchdog check interval. Accepts units, e.g. 1s, 500ms")
            .default_value(std::string("1"));
        p.add_argument("--watchdog-warning-threshold")
            .help(
                "Watchdog long-running task warning threshold. Accepts units, "
                "e.g. 300s, 5m")
            .default_value(std::string("300"));
        p.add_argument("--watchdog-idle-timeout")
            .help("Watchdog idle timeout (0 = use default). Accepts units")
            .default_value(std::string("300"));
        p.add_argument("--watchdog-deadlock-timeout")
            .help("Watchdog deadlock timeout (0 = use default). Accepts units")
            .default_value(std::string("600"));
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        disable = p.get<bool>("--disable-watchdog");
        global_timeout = seconds_int(p, "--watchdog-global-timeout");
        task_timeout = seconds_int(p, "--watchdog-task-timeout");
        interval = seconds_int(p, "--watchdog-interval");
        warning_threshold = seconds_int(p, "--watchdog-warning-threshold");
        idle_timeout = seconds_int(p, "--watchdog-idle-timeout");
        deadlock_timeout = seconds_int(p, "--watchdog-deadlock-timeout");
    }

   private:
    static int seconds_int(const argparse::ArgumentParser& p,
                           const char* name) {
        return static_cast<int>(std::lround(get_seconds_arg(p, name)));
    }

   public:
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
};

inline PipelineConfig build_pipeline_config(const std::string& name,
                                            const PipelineArgs& pipeline) {
    auto config = PipelineConfig().with_name(name).with_watchdog(false);
    pipeline.apply(config);
    return config;
}

inline PipelineConfig build_pipeline_config(const std::string& name,
                                            const PipelineArgs& pipeline,
                                            const WatchdogArgs& watchdog) {
    auto config = PipelineConfig().with_name(name);
    pipeline.apply(config);
    watchdog.apply(config);
    return config;
}

// A DFTracer trace file: .pfw or .pfw.gz.
inline bool is_trace_file(const std::string& path) {
    return (path.size() >= 4 &&
            path.compare(path.size() - 4, 4, ".pfw") == 0) ||
           (path.size() >= 7 &&
            path.compare(path.size() - 7, 7, ".pfw.gz") == 0);
}

// Split a comma-separated list, dropping empty fields.
inline std::vector<std::string> split_csv(const std::string& str) {
    std::vector<std::string> out;
    if (str.empty()) return out;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

// Warn when an aggregated workload of `required_bytes` will not fit in one
// process (peak is ~PEAK_MEMORY_FACTOR x that); the message says how much
// memory or how many nodes it needs. `available_bytes = 0` detects it
// (cgroup-aware). The Python/dfanalyzer path emits the same message via
// memory_budget_advice.
inline void warn_if_memory_tight(std::size_t required_bytes,
                                 std::size_t available_bytes = 0) {
    const auto advice =
        dftracer::utils::memory_budget_advice(required_bytes, available_bytes);
    if (!advice.fits) {
        DFTRACER_UTILS_LOG_WARN(
            "%s",
            dftracer::utils::format_memory_budget_warning(advice).c_str());
    }
}

// Parallel per-subdirectory scan of `directory` for .pfw/.pfw.gz files via
// ctx.spawn (context lets a recursive walk fan out per subdirectory); sizes
// are not populated. Returns the matched paths.
inline coro::CoroTask<std::vector<std::string>> scan_directory_trace_files(
    CoroScope& ctx, const std::string& directory, bool recursive) {
    utilities::filesystem::PatternDirectoryScannerUtility scanner;
    utilities::filesystem::PatternDirectoryScannerUtilityInput scan_input{
        directory, {".pfw", ".pfw.gz"}, recursive};
    auto matched = co_await scanner(ctx, scan_input);
    std::vector<std::string> files;
    files.reserve(matched.size());
    for (const auto& entry : matched) {
        files.push_back(entry.path.string());
    }
    co_return files;
}

// Enumerate trace files from input paths (each a file or directory).
// Directories are scanned via the parallel scanner above; results are sorted
// for deterministic output. `on_missing(path)` runs for inputs that are
// neither a regular file nor a directory (template param so it inlines).
template <class OnMissing>
coro::CoroTask<std::vector<std::string>> collect_input_trace_files(
    CoroScope& ctx, const std::vector<std::string>& inputs, bool recursive,
    OnMissing&& on_missing) {
    std::vector<std::string> out;
    for (const auto& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            auto scanned =
                co_await scan_directory_trace_files(ctx, in, recursive);
            out.insert(out.end(), std::make_move_iterator(scanned.begin()),
                       std::make_move_iterator(scanned.end()));
        } else if (fs::is_regular_file(in, ec)) {
            out.push_back(in);
        } else {
            on_missing(in);
        }
    }
    std::sort(out.begin(), out.end());
    co_return out;
}

inline coro::CoroTask<std::vector<std::string>> collect_input_trace_files(
    CoroScope& ctx, const std::vector<std::string>& inputs, bool recursive) {
    co_return co_await collect_input_trace_files(ctx, inputs, recursive,
                                                 [](const std::string&) {});
}

// Run `body` as a single-node pipeline and return its int result. `body` is a
// template param (inlinable, no type erasure); it runs exactly once.
template <class Body>
int run_single_task(const std::string& name, const PipelineArgs& pipeline,
                    Body&& body) {
    Pipeline p(build_pipeline_config(name, pipeline));
    auto task =
        make_task([body = std::forward<Body>(body)](CoroScope& ctx)
                      -> coro::CoroTask<int> { co_return co_await body(ctx); },
                  name);
    p.set_source(task);
    p.set_destination(task);
    p.execute();
    return task->template get<int>();
}

// Split out so the run_single_task lambda below holds no coroutine locals,
// which ICEs GCC 12/13.
inline coro::CoroTask<int> ensure_indexes_fresh_task(
    CoroScope& ctx, const std::string& directory,
    const std::vector<std::string>& files, const std::string& index_dir,
    bool force_rebuild) {
    co_await trace::indexing::ensure_indexes_fresh(&ctx, directory, files,
                                                   index_dir, force_rebuild);
    co_return 0;
}

inline int ensure_indexes_fresh_blocking(const std::string& name,
                                         const PipelineArgs& pipeline,
                                         const std::string& directory,
                                         std::vector<std::string> files,
                                         const std::string& index_dir,
                                         bool force_rebuild = false) {
    return run_single_task(
        name, pipeline, [&](CoroScope& ctx) -> coro::CoroTask<int> {
            co_return co_await ensure_indexes_fresh_task(
                ctx, directory, files, index_dir, force_rebuild);
        });
}

// Channel-backed fan-out: a single bounded producer feeds `items` (moved in)
// to `threads` consumers, each running `co_await worker(item)`. `worker` is a
// template param so it inlines on the work path (never std::function). Spawns
// onto `scope`; the caller's enclosing scope waits for completion.
template <class Range, class Worker>
void parallel_for_each(CoroScope& scope, Range items, std::size_t threads,
                       Worker worker) {
    using Item = std::decay_t<decltype(*std::begin(items))>;
    auto chan = coro::make_channel<Item>(threads * 2);
    scope.spawn([ch = chan->producer(), items = std::move(items)](
                    CoroScope&) mutable -> coro::CoroTask<void> {
        auto guard = ch.guard();
        for (const auto& item : items) {
            if (!co_await ch.send(item)) co_return;
        }
        co_return;
    });
    for (std::size_t w = 0; w < threads; ++w) {
        scope.spawn([ch = chan->consumer(),
                     worker](CoroScope&) mutable -> coro::CoroTask<void> {
            while (auto item = co_await ch.receive()) {
                co_await worker(*item);
            }
            co_return;
        });
    }
}

}  // namespace dftracer::utils::cli

#endif  // DFTRACER_UTILS_BINARIES_COMMON_CLI_H
