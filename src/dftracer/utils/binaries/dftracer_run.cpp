#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::views;
using namespace dftracer::utils::utilities::filesystem;
using dftracer::utils::plugins::ConfigTree;
using dftracer::utils::plugins::Plugins;

struct PluginBlock {
    std::string path;
    std::string pconfig;
    std::vector<std::pair<std::string, std::string>> pargs;
};

// Config applied under every plugin block.
struct SharedConfig {
    std::string pconfig;
    std::vector<std::pair<std::string, std::string>> pargs;
};

struct PluginArgs {
    std::vector<PluginBlock> blocks;
    SharedConfig shared;
};

static std::pair<std::string, std::string> split_kv(const std::string& kv) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) return {kv, std::string()};
    return {kv.substr(0, eq), kv.substr(eq + 1)};
}

// Pull the plugin/config flags into `out`, keeping the rest in `kept` for
// argparse; false on misuse (already logged).
static bool prescan_plugin_args(int argc, char** argv, std::vector<char*>& kept,
                                PluginArgs& out) {
    kept.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take_value = [&](const char* flag, std::string& value) -> bool {
            if (i + 1 >= argc) {
                DFTRACER_UTILS_LOG_ERROR("%s requires a value", flag);
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (arg == "--plugin") {
            std::string path;
            if (!take_value("--plugin", path)) return false;
            out.blocks.push_back(PluginBlock{path, {}, {}});
        } else if (arg.rfind("--plugin=", 0) == 0) {
            out.blocks.push_back(PluginBlock{arg.substr(9), {}, {}});
        } else if (arg == "--parg" || arg.rfind("--parg=", 0) == 0) {
            std::string kv;
            if (arg == "--parg") {
                if (!take_value("--parg", kv)) return false;
            } else {
                kv = arg.substr(7);
            }
            if (out.blocks.empty()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "%s",
                    "--parg must follow a --plugin; use --shared-parg for "
                    "shared args");
                return false;
            }
            out.blocks.back().pargs.push_back(split_kv(kv));
        } else if (arg == "--pconfig" || arg.rfind("--pconfig=", 0) == 0) {
            std::string file;
            if (arg == "--pconfig") {
                if (!take_value("--pconfig", file)) return false;
            } else {
                file = arg.substr(10);
            }
            if (out.blocks.empty()) {
                DFTRACER_UTILS_LOG_ERROR("%s",
                                         "--pconfig must follow a --plugin; "
                                         "use --shared-pconfig for "
                                         "a shared config file");
                return false;
            }
            out.blocks.back().pconfig = file;
        } else if (arg == "--shared-parg" ||
                   arg.rfind("--shared-parg=", 0) == 0) {
            std::string kv;
            if (arg == "--shared-parg") {
                if (!take_value("--shared-parg", kv)) return false;
            } else {
                kv = arg.substr(14);
            }
            out.shared.pargs.push_back(split_kv(kv));
        } else if (arg == "--shared-pconfig" ||
                   arg.rfind("--shared-pconfig=", 0) == 0) {
            std::string file;
            if (arg == "--shared-pconfig") {
                if (!take_value("--shared-pconfig", file)) return false;
            } else {
                file = arg.substr(17);
            }
            out.shared.pconfig = file;
        } else {
            kept.push_back(argv[i]);
        }
    }
    return true;
}

class RunArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_EMPTY};
    cli::FilesArgs files_args;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::WatchdogArgs watchdog;

    bool no_auto_index = false;
    bool describe = false;

    explicit RunArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.with_force = false;
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        schema(directory, files_args, pipeline, indexing, watchdog);
    }

   protected:
    void register_args() override {
        // The plugin/config flags are extracted by prescan_plugin_args before
        // argparse runs.
        parser()
            .add_argument("--no-auto-index")
            .help(
                "Disable automatic index building for files missing .dftindex")
            .flag();
        parser()
            .add_argument("--describe")
            .help(
                "Load --plugin libraries, print what each provides/consumes, "
                "and exit without scanning")
            .flag();
    }

    void post_parse() override {
        no_auto_index = parser().get<bool>("--no-auto-index");
        describe = parser().get<bool>("--describe");
    }
};

// Layer config low to high: shared pconfig, shared pargs, block pconfig, block
// pargs.
static ConfigTree build_config(const SharedConfig& shared,
                               const PluginBlock& block) {
    ConfigTree tree;
    if (!shared.pconfig.empty())
        tree.merge_from(ConfigTree::from_json_file(shared.pconfig));
    for (const auto& [k, v] : shared.pargs) tree.set(k, v);
    if (!block.pconfig.empty())
        tree.merge_from(ConfigTree::from_json_file(block.pconfig));
    for (const auto& [k, v] : block.pargs) tree.set(k, v);
    return tree;
}

static bool block_has_config(const SharedConfig& shared,
                             const PluginBlock& block) {
    return !shared.pconfig.empty() || !shared.pargs.empty() ||
           !block.pconfig.empty() || !block.pargs.empty();
}

// Build the plugin set from --plugin/--pconfig/--parg blocks. Shared by
// --describe and the real run so both go through the same load-time
// validation (ABI gate, reserved-name refusal, duplicate-provides, unmet
// consumes).
static Result<Plugins> build_plugins(const PluginArgs& plugin_args) {
    auto builder = Plugins::builder();
    for (const auto& block : plugin_args.blocks) {
        if (block_has_config(plugin_args.shared, block)) {
            try {
                builder.add(block.path,
                            build_config(plugin_args.shared, block));
            } catch (const std::exception& e) {
                return make_error(
                    ErrorCode::INVALID_ARGUMENT,
                    "plugin '" + block.path + "' config error: " + e.what());
            }
        } else {
            builder.add(block.path);
        }
    }
    return builder.build();
}

static std::string join_or_none(const std::vector<std::string>& names) {
    if (names.empty()) return "(none)";
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

// Print each loaded plugin's path and provides/consumes, one line per fact so
// the output is greppable, and exit without touching any trace file.
static int describe_plugins(const PluginArgs& plugin_args) {
    if (plugin_args.blocks.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "No plugins specified. Use --plugin path.so.");
        return 1;
    }

    auto plugins = build_plugins(plugin_args);
    if (!plugins) {
        DFTRACER_UTILS_LOG_ERROR("%s", plugins.error().format().c_str());
        return 1;
    }

    for (const auto& info : plugins->describe()) {
        std::fprintf(stderr, "plugin: %s\n", info.path.c_str());
        std::fprintf(stderr, "  abi_version: %u\n", info.abi_version);
        std::fprintf(stderr, "  plan_query: %s\n",
                     info.has_plan_query ? "yes" : "no");
        std::fprintf(stderr, "  provides: %s\n",
                     join_or_none(info.provides).c_str());
        std::fprintf(stderr, "  consumes: %s\n",
                     join_or_none(info.consumes).c_str());
        std::fprintf(stderr, "  ops: %s\n", join_or_none(info.ops).c_str());
        // "(none)" here means the plugin declared no projection, so it is
        // handed every batch column - not that it reads nothing.
        std::fprintf(stderr, "  reads: %s\n",
                     info.reads.empty() ? "(all columns)"
                                        : join_or_none(info.reads).c_str());
        if (info.config_keys.empty()) {
            std::fprintf(stderr, "  config: (undeclared)\n");
        } else {
            for (const auto& key : info.config_keys)
                std::fprintf(stderr, "  config: %s\n", key.c_str());
        }
        std::fprintf(stderr, "\n");
    }
    return 0;
}

static coro::CoroTask<int> run_plugins(const RunArgParse* cli,
                                       const PluginArgs* plugin_args) {
    const auto& directory = cli->directory.value;
    const auto& index_dir = cli->indexing.index_dir;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const bool no_auto_index = cli->no_auto_index;

    if (plugin_args->blocks.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "No plugins specified. Use --plugin path.so.");
        co_return 1;
    }

    std::vector<std::string> files;
    if (!directory.empty()) {
        PatternDirectoryScannerUtility scanner;
        PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
        auto matched = co_await scanner(scan_input);
        for (const auto& entry : matched) files.push_back(entry.path.string());
    } else {
        files = cli->files_args.value;
    }

    if (files.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No .pfw or .pfw.gz files found. Use -d or --files.");
        co_return 1;
    }

    // Normalize single-member inputs to multi-member gzip so each checkpoint is
    // a real member.
    if (!no_auto_index) {
        auto norm = co_await indexing::normalize_members_for_ingest(
            std::move(files), checkpoint_size);
        files = std::move(norm.files);
    }

    // A load or fold-ordering failure must stop the run before any scanning.
    auto plugins = build_plugins(*plugin_args);
    if (!plugins) {
        DFTRACER_UTILS_LOG_ERROR("%s", plugins.error().format().c_str());
        co_return 1;
    }

    std::vector<ViewFile> view_files;
    view_files.reserve(files.size());
    for (const auto& file_path : files) {
        ViewFile vf;
        vf.file_path = file_path;
        vf.index_path = internal::determine_index_path(file_path, index_dir);
        vf.checkpoint_size = checkpoint_size;
        view_files.push_back(std::move(vf));
    }

    ExportStats stats;
    auto pipeline_config = cli::build_pipeline_config(
        "DFTracer Run", cli->pipeline, cli->watchdog);
    Pipeline pipeline(pipeline_config);
    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            if (!no_auto_index)
                co_await indexing::ensure_indexes_fresh(&ctx, "", files,
                                                        index_dir);
            View view = View::from_files(view_files);
            auto run = co_await plugins->run(view);
            if (!run) {
                DFTRACER_UTILS_LOG_ERROR("%s", run.error().format().c_str());
                co_return;
            }
            stats = run->stats;
            co_return;
        },
        "DFTracerRun");
    pipeline.set_source(task);
    pipeline.set_destination(task);
    try {
        pipeline.execute();
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Pipeline failed: %s", e.what());
        co_return 1;
    }

    std::fprintf(stderr,
                 "Run: plugins=%zu | Files: %zu | Chunks: scanned=%llu "
                 "skipped=%llu | Events: matched=%llu scanned=%llu\n",
                 plugins->size(), files.size(),
                 (unsigned long long)stats.chunks_scanned,
                 (unsigned long long)stats.chunks_skipped,
                 (unsigned long long)stats.events_matched,
                 (unsigned long long)stats.events_scanned);
    co_return 0;
}

int main(int argc, char** argv) {
    PluginArgs plugin_args;
    std::vector<char*> kept;
    kept.reserve(static_cast<std::size_t>(argc));
    if (!prescan_plugin_args(argc, argv, kept, plugin_args)) return 1;

    return cli::cli_main<RunArgParse>(
        static_cast<int>(kept.size()), kept.data(), "dftracer_run",
        "Run compiled analysis plugins over DFTracer trace files. Each "
        "--plugin shared library is loaded and run as a fold over one shared, "
        "index-pruned parallel scan. Add per-plugin config with --pconfig FILE "
        "and --parg KEY=VALUE (after a --plugin), or shared config with "
        "--shared-pconfig FILE and --shared-parg KEY=VALUE. --describe loads "
        "the plugins and reports what each provides/consumes without "
        "scanning.",
        [&plugin_args](RunArgParse& cli) -> int {
            if (cli.describe) return describe_plugins(plugin_args);
            try {
                return run_plugins(&cli, &plugin_args).get();
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Fatal: %s", e.what());
                return 1;
            }
        });
}
