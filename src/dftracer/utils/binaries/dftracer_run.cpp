// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdio>
#include <exception>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::views;
using dftracer::utils::plugins::ConfigTree;
using dftracer::utils::plugins::Plugins;

struct PluginBlock {
    dstd::StringView path;
    dstd::StringView pconfig;
    dstd::Vector<dstd::Pair<dstd::StringView, dstd::StringView>> pargs;
};

// Config applied under every plugin block.
struct SharedConfig {
    dstd::StringView pconfig;
    dstd::Vector<dstd::Pair<dstd::StringView, dstd::StringView>> pargs;
};

struct PluginArgs {
    dstd::Vector<PluginBlock> blocks;
    SharedConfig shared;
};

static dstd::Pair<dstd::StringView, dstd::StringView> split_kv(
    const dstd::StringView& kv) {
    const auto eq = kv.find('=');
    if (eq == dstd::StringView::npos) return {kv, dstd::StringView()};
    return {kv.substr(0, eq), kv.substr(eq + 1)};
}

// Pull the plugin/config flags into `out`, keeping the rest in `kept` for
// the option parser; false on misuse (already logged).
static bool prescan_plugin_args(int argc, char** argv,
                                dstd::Vector<char*>& kept, PluginArgs& out) {
    kept.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const dstd::StringView arg = argv[i];
        auto take_value = [&](const char* flag,
                              dstd::StringView& value) -> bool {
            if (i + 1 >= argc) {
                DFTRACER_UTILS_LOG_ERROR("{} requires a value", flag);
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (arg == "--plugin") {
            dstd::StringView path;
            if (!take_value("--plugin", path)) return false;
            out.blocks.push_back(PluginBlock{path, {}, {}});
        } else if (arg.starts_with("--plugin=")) {
            out.blocks.push_back(PluginBlock{arg.substr(9), {}, {}});
        } else if (arg == "--parg" || arg.starts_with("--parg=")) {
            dstd::StringView kv;
            if (arg == "--parg") {
                if (!take_value("--parg", kv)) return false;
            } else {
                kv = arg.substr(7);
            }
            if (out.blocks.empty()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "{}",
                    "--parg must follow a --plugin; use --shared-parg for "
                    "shared args");
                return false;
            }
            out.blocks.back().pargs.push_back(split_kv(kv));
        } else if (arg == "--pconfig" || arg.starts_with("--pconfig=")) {
            dstd::StringView file;
            if (arg == "--pconfig") {
                if (!take_value("--pconfig", file)) return false;
            } else {
                file = arg.substr(10);
            }
            if (out.blocks.empty()) {
                DFTRACER_UTILS_LOG_ERROR("{}",
                                         "--pconfig must follow a --plugin; "
                                         "use --shared-pconfig for "
                                         "a shared config file");
                return false;
            }
            out.blocks.back().pconfig = file;
        } else if (arg == "--shared-parg" ||
                   arg.starts_with("--shared-parg=")) {
            dstd::StringView kv;
            if (arg == "--shared-parg") {
                if (!take_value("--shared-parg", kv)) return false;
            } else {
                kv = arg.substr(14);
            }
            out.shared.pargs.push_back(split_kv(kv));
        } else if (arg == "--shared-pconfig" ||
                   arg.starts_with("--shared-pconfig=")) {
            dstd::StringView file;
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

class RunArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{binaries::DirMode::DefaultEmpty,
                               "Trace files or a folder of trace files"};
    binaries::RuntimeArgs runtime_args;
    binaries::IndexingArgs indexing;
    binaries::WatchdogArgs watchdog;

    bool no_auto_index = false;
    bool describe = false;

    explicit RunArgParse(binaries::ArgParser& p) : ArgParse(p) {
        indexing.with_force = false;
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        schema(inputs, runtime_args, indexing, watchdog);
    }

   protected:
    void register_args() override {
        // The plugin/config flags are extracted by prescan_plugin_args before
        // the option parser runs.
        parser().flag(
            no_auto_index, {"--no-auto-index"},
            "Disable automatic index building for files missing .dftindex");
        parser().flag(describe, {"--describe"},
                      "Load --plugin libraries, print what each "
                      "provides/consumes, and exit without scanning");
    }
};

// Layer config low to high: shared pconfig, shared pargs, block pconfig, block
// pargs.
static ConfigTree build_config(const SharedConfig& shared,
                               const PluginBlock& block) {
    ConfigTree tree;
    if (!shared.pconfig.empty())
        tree.merge_from(
            ConfigTree::from_json_file(dstd::String(shared.pconfig)));
    for (const auto& [k, v] : shared.pargs)
        tree.set(dstd::String(k), dstd::String(v));
    if (!block.pconfig.empty())
        tree.merge_from(
            ConfigTree::from_json_file(dstd::String(block.pconfig)));
    for (const auto& [k, v] : block.pargs)
        tree.set(dstd::String(k), dstd::String(v));
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
static dstd::Result<Plugins> build_plugins(const PluginArgs& plugin_args) {
    auto builder = Plugins::builder();
    for (const auto& block : plugin_args.blocks) {
        if (block_has_config(plugin_args.shared, block)) {
            try {
                builder.add(dstd::String(block.path),
                            build_config(plugin_args.shared, block));
            } catch (const std::exception& e) {
                return common::make_error(
                    common::ErrorCode::InvalidArgument,
                    common::str_cat("plugin '", block.path,
                                    "' config error: ", e.what()));
            }
        } else {
            builder.add(dstd::String(block.path));
        }
    }
    return builder.build();
}

static dstd::String join_or_none(const dstd::Vector<dstd::String>& names) {
    if (names.empty()) return "(none)";
    dstd::String out;
    for (const auto& name : names) {
        if (!out.empty()) out += ", ";
        out.append(name.data(), name.size());
    }
    return out;
}

// Print each loaded plugin's path and provides/consumes, one line per fact so
// the output is greppable, and exit without touching any trace file.
static int describe_plugins(const PluginArgs& plugin_args) {
    if (plugin_args.blocks.empty()) {
        DFTRACER_UTILS_LOG_ERROR("{}",
                                 "No plugins specified. Use --plugin path.so.");
        return 1;
    }

    auto plugins = build_plugins(plugin_args);
    if (!plugins) {
        DFTRACER_UTILS_LOG_ERROR("{}", plugins.error().format());
        return 1;
    }

    for (const auto& info : plugins->describe()) {
        std::fprintf(stderr, "plugin: %s\n", info.path.c_str());
        std::fprintf(stderr, "  abi_version: %u\n", info.abi_version);
        std::fprintf(stderr, "  plan_duql: %s\n",
                     info.has_plan_duql ? "yes" : "no");
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
    const dstd::String directory(cli->inputs.directory);
    const dstd::String index_dir(cli->indexing.index_dir);
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const bool no_auto_index = cli->no_auto_index;

    if (plugin_args->blocks.empty()) {
        DFTRACER_UTILS_LOG_ERROR("{}",
                                 "No plugins specified. Use --plugin path.so.");
        co_return 1;
    }

    dstd::Vector<dstd::String> files;
    if (!directory.empty()) {
        auto matched =
            binaries::scan_trace_files_on_pool(cli->runtime_args, directory);
        for (const auto& entry : matched) files.emplace_back(entry.path.str());
    }
    for (const auto file : cli->inputs.files) files.emplace_back(file);

    if (files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("{}", "No trace files found.");
        co_return 1;
    }

    // A load or fold-ordering failure must stop the run before any scanning.
    auto plugins = build_plugins(*plugin_args);
    if (!plugins) {
        DFTRACER_UTILS_LOG_ERROR("{}", plugins.error().format());
        co_return 1;
    }

    dstd::Vector<ViewFile> view_files;
    view_files.reserve(files.size());
    for (const auto& file_path : files) {
        ViewFile vf;
        vf.file_path.assign(file_path.data(), file_path.size());
        const dstd::String index_path =
            internal::determine_index_path(file_path, index_dir);
        vf.index_path.assign(index_path.data(), index_path.size());
        vf.checkpoint_size = checkpoint_size;
        view_files.push_back(std::move(vf));
    }

    ExportStats stats;
    try {
        binaries::run_on_pool(
            "DFTracer Run", cli->runtime_args, cli->watchdog,
            [&](coro::Scope&) -> coro::CoroTask<void> {
                if (!no_auto_index)
                    common::unwrap(
                        co_await dftracer::utils::index::build::
                            ensure_indexes_fresh("", files, index_dir));
                View view = View::from_files(view_files);
                auto run = co_await plugins->run(view);
                if (!run) {
                    DFTRACER_UTILS_LOG_ERROR("{}", run.error().format());
                    co_return;
                }
                stats = run->stats;
                co_return;
            });
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Run failed: {}", e.what());
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
    dstd::Vector<char*> kept;
    kept.reserve(static_cast<std::size_t>(argc));
    if (!prescan_plugin_args(argc, argv, kept, plugin_args)) return 1;

    return binaries::cli_main<RunArgParse>(
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
                auto code = run_plugins(&cli, &plugin_args).get();
                if (!code) {
                    DFTRACER_UTILS_LOG_ERROR("Fatal: {}",
                                             code.error().format());
                    return 1;
                }
                return *code;
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Fatal: {}", e.what());
                return 1;
            }
        });
}
