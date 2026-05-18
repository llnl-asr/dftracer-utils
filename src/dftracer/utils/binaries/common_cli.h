#ifndef DFTRACER_UTILS_BINARIES_COMMON_CLI_H
#define DFTRACER_UTILS_BINARIES_COMMON_CLI_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace dftracer::utils::cli {

class ArgParse;

struct CliSchema {
    virtual ~CliSchema() = default;
    virtual void register_on(argparse::ArgumentParser& p) = 0;
    virtual void parse_from(const argparse::ArgumentParser& p) = 0;
    virtual bool validate() { return true; }
};

class ArgParse {
   public:
    explicit ArgParse(argparse::ArgumentParser& parser) : parser_(parser) {}
    virtual ~ArgParse() = default;

    ArgParse(const ArgParse&) = delete;
    ArgParse& operator=(const ArgParse&) = delete;

    void setup() {
        for (auto* s : schemas_) s->register_on(parser_);
        register_args();
    }

    bool parse(int argc, char** argv) {
        try {
            parser_.parse_args(argc, argv);
        } catch (const std::exception& err) {
            DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
            std::fprintf(stderr, "%s\n", parser_.help().str().c_str());
            return false;
        }
        for (auto* s : schemas_) s->parse_from(parser_);
        post_parse();
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

    PipelineArgs() = default;

    void register_on(argparse::ArgumentParser& p) override {
        p.add_group("Pipeline");
        p.add_argument("--executor-threads")
            .help(
                "Number of worker threads for parallel processing "
                "(default: number of CPU cores)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(
                dftracer_utils_hardware_concurrency()));
        p.add_argument("--io-threads")
            .help(
                "Number of I/O threads "
                "(default: number of CPU cores)")
            .scan<'d', std::size_t>()
            .default_value(dftracer_utils_hardware_concurrency());
        p.add_argument("--time-profiling")
            .help("Print stage timing breakdown to stderr")
            .flag();
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        executor_threads = p.get<std::size_t>("--executor-threads");
        io_threads = p.get<std::size_t>("--io-threads");
        time_profiling = p.get<bool>("--time-profiling");
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
            .help("Checkpoint size for gzip indexing in bytes (default: " +
                  std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE) +
                  ")")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(
                constants::indexer::DEFAULT_CHECKPOINT_SIZE));
        if (with_force) {
            p.add_argument("-f", "--force").help(force_help).flag();
        }
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        if (with_index_dir) {
            index_dir = p.get<std::string>("--index-dir");
        }
        checkpoint_size = p.get<std::size_t>("--checkpoint-size");
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
                "Watchdog global timeout for pipeline execution in "
                "seconds (0 = no timeout)")
            .scan<'d', int>()
            .default_value(0);
        p.add_argument("--watchdog-task-timeout")
            .help("Watchdog default task timeout in seconds (0 = no timeout)")
            .scan<'d', int>()
            .default_value(0);
        p.add_argument("--watchdog-interval")
            .help("Watchdog check interval in seconds")
            .scan<'d', int>()
            .default_value(1);
        p.add_argument("--watchdog-warning-threshold")
            .help("Watchdog long-running task warning threshold in seconds")
            .scan<'d', int>()
            .default_value(300);
        p.add_argument("--watchdog-idle-timeout")
            .help("Watchdog idle timeout in seconds (0 = use default)")
            .scan<'d', int>()
            .default_value(300);
        p.add_argument("--watchdog-deadlock-timeout")
            .help("Watchdog deadlock timeout in seconds (0 = use default)")
            .scan<'d', int>()
            .default_value(600);
    }

    void parse_from(const argparse::ArgumentParser& p) override {
        disable = p.get<bool>("--disable-watchdog");
        global_timeout = p.get<int>("--watchdog-global-timeout");
        task_timeout = p.get<int>("--watchdog-task-timeout");
        interval = p.get<int>("--watchdog-interval");
        warning_threshold = p.get<int>("--watchdog-warning-threshold");
        idle_timeout = p.get<int>("--watchdog-idle-timeout");
        deadlock_timeout = p.get<int>("--watchdog-deadlock-timeout");
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

}  // namespace dftracer::utils::cli

#endif  // DFTRACER_UTILS_BINARIES_COMMON_CLI_H
