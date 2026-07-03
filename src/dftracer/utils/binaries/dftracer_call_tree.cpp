// Pipeline-driven call_tree binary.
//
// DAG:
//   scan -> build -> merge -> hierarchy -> write_json
//
// scan      : enumerate inputs
// build     : per-file CoroScope fan-out; each file ingests into its own
//             local CallTree fragment (no shared mutation)
// merge     : concatenate fragments into ctx.merged
// hierarchy : per-process CoroScope fan-out; each ProcessCallTree is
//             independent so parent-child build runs in parallel
// write_json: per-worker serialization of process slices, ParallelWriter
//             feeds io_backend for the actual writes

#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/internal/trace_reader.h>
#include <dftracer/utils/call_tree/json_serializer.h>
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::call_tree;

namespace {

class CallTreeArgParse : public cli::ArgParse {
   public:
    cli::PipelineArgs pipeline;

    std::vector<std::string> inputs;
    bool recursive = false;
    std::string output;
    bool no_save = false;
    bool gzip = false;

    explicit CallTreeArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(pipeline);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("inputs")
            .help("Trace files (.pfw, .pfw.gz) or directories")
            .nargs(argparse::nargs_pattern::at_least_one);
        parser().add_argument("-r", "--recursive").flag();
        parser()
            .add_argument("-o", "--output")
            .help("Output JSON path (Chrome Tracing)")
            .default_value<std::string>("");
        parser().add_argument("--no-save").flag();
        parser()
            .add_argument("--gzip")
            .help("gzip the output (.gz appended if needed)")
            .flag();
    }

    void post_parse() override {
        inputs = parser().get<std::vector<std::string>>("inputs");
        recursive = parser().get<bool>("--recursive");
        output = parser().get<std::string>("--output");
        no_save = parser().get<bool>("--no-save");
        gzip = parser().get<bool>("--gzip");
    }
};

struct RunCtx {
    const CallTreeArgParse* cli = nullptr;

    std::vector<std::string> trace_files;
    std::vector<std::unique_ptr<internal::CallTree>> per_file;
    internal::CallTree merged;
    std::vector<internal::ProcessKey> process_keys;

    std::string output_path;
    bool failed = false;

    double scan_ms = 0;
    double build_ms = 0;
    double merge_ms = 0;
    double hier_ms = 0;
    double write_ms = 0;
};

coro::CoroTask<void> task_scan(RunCtx* ctx, CoroScope& scope) {
    const auto t0 = std::chrono::steady_clock::now();
    ctx->trace_files = co_await cli::collect_input_trace_files(
        scope, ctx->cli->inputs, ctx->cli->recursive);
    if (ctx->trace_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "no trace files found");
        ctx->failed = true;
    }
    ctx->scan_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    if (!ctx->failed) {
        DFTRACER_UTILS_LOG_DEBUG("[scan] %.2f ms: %zu files", ctx->scan_ms,
                                 ctx->trace_files.size());
    }
    co_return;
}

coro::CoroTask<void> ingest_one_file(std::string path, internal::CallTree* tree,
                                     std::atomic<std::size_t>* total) {
    auto counts = co_await internal::read_trace_file_async(std::move(path),
                                                           tree, nullptr);
    total->fetch_add(counts.processed, std::memory_order_relaxed);
}

coro::CoroTask<void> ingest_all_files(
    CoroScope* child, const std::vector<std::string>* paths,
    const std::vector<std::unique_ptr<internal::CallTree>>* per_file,
    std::atomic<std::size_t>* total) {
    for (std::size_t i = 0; i < paths->size(); ++i) {
        std::string path = (*paths)[i];
        internal::CallTree* tree = (*per_file)[i].get();
        child->spawn([path = std::move(path), tree,
                      total](CoroScope&) mutable -> coro::CoroTask<void> {
            co_await ingest_one_file(std::move(path), tree, total);
        });
    }
    co_return;
}

coro::CoroTask<void> task_build(RunCtx* ctx, CoroScope* scope) {
    if (ctx->failed) co_return;
    const auto t0 = std::chrono::steady_clock::now();

    const std::size_t n = ctx->trace_files.size();
    ctx->per_file.clear();
    ctx->per_file.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ctx->per_file.push_back(std::make_unique<internal::CallTree>());
        ctx->per_file.back()->initialize();
    }

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t>* total_ptr = &total_events;

    const std::vector<std::string>* paths_ptr = &ctx->trace_files;
    const std::vector<std::unique_ptr<internal::CallTree>>* per_file_ptr =
        &ctx->per_file;

    co_await scope->scope(
        [paths_ptr, per_file_ptr,
         total_ptr](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await ingest_all_files(&child, paths_ptr, per_file_ptr,
                                      total_ptr);
        });

    ctx->build_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    DFTRACER_UTILS_LOG_DEBUG("[build] %.2f ms: %zu events across %zu files",
                             ctx->build_ms, total_events.load(), n);
    co_return;
}

coro::CoroTask<void> task_merge(RunCtx* ctx) {
    if (ctx->failed) co_return;
    const auto t0 = std::chrono::steady_clock::now();
    ctx->merged.initialize();
    for (auto& t : ctx->per_file) {
        if (t) ctx->merged.merge_from(std::move(*t));
    }
    ctx->per_file.clear();
    ctx->process_keys = ctx->merged.keys();
    ctx->merge_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    DFTRACER_UTILS_LOG_DEBUG("[merge] %.2f ms: %zu processes", ctx->merge_ms,
                             ctx->process_keys.size());
    co_return;
}

coro::CoroTask<void> hier_one_process(internal::CallTree* tree,
                                      internal::ProcessKey key) {
    tree->build_hierarchy_for_process(key);
    co_return;
}

coro::CoroTask<void> hier_all_processes(
    CoroScope* child, internal::CallTree* tree,
    const std::vector<internal::ProcessKey>* keys) {
    for (auto k : *keys) {
        child->spawn([tree, k](CoroScope&) mutable -> coro::CoroTask<void> {
            co_await hier_one_process(tree, k);
        });
    }
    co_return;
}

coro::CoroTask<void> task_hierarchy(RunCtx* ctx, CoroScope* scope) {
    if (ctx->failed) co_return;
    const auto t0 = std::chrono::steady_clock::now();

    internal::CallTree* tree = &ctx->merged;
    const std::vector<internal::ProcessKey>* keys_ptr = &ctx->process_keys;
    co_await scope->scope(
        [tree, keys_ptr](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await hier_all_processes(&child, tree, keys_ptr);
        });

    ctx->hier_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    DFTRACER_UTILS_LOG_DEBUG("[hierarchy] %.2f ms", ctx->hier_ms);
    co_return;
}

// Serialize all events for a single process slice into `out`. Each event is
// followed by ",\n"; trim the final separator at concatenation time.
void serialize_process_slice(const internal::ProcessCallTree& pgraph,
                             const internal::ProcessKey& key,
                             internal::JsonSerializer& serializer,
                             std::size_t starting_index, std::string& out) {
    static constexpr std::size_t EVT_BUF = 16384;
    char buffer[EVT_BUF];
    std::size_t event_idx = starting_index;
    for (std::uint64_t root_id : pgraph.root_calls) {
        std::vector<std::uint64_t> stack;
        stack.push_back(root_id);
        while (!stack.empty()) {
            std::uint64_t node_id = stack.back();
            stack.pop_back();
            auto it = pgraph.calls.find(node_id);
            if (it == pgraph.calls.end()) continue;
            const auto& node = it->second;
            std::size_t written = serializer.serialize_node(
                buffer, static_cast<int>(event_idx++), *node, key.pid, key.tid);
            // serialize_node returns size including trailing newline; strip it
            // and add ",\n" so concatenation produces valid
            // JSON-array-of-lines.
            if (written > 0) {
                out.append(buffer, written - 1);
                out.append(",\n", 2);
            }
            const auto& children = node->get_children();
            for (auto cit = children.rbegin(); cit != children.rend(); ++cit) {
                stack.push_back(*cit);
            }
        }
    }
}

coro::CoroTask<void> serialize_slice(const internal::CallTree* merged,
                                     internal::ProcessKey key,
                                     const std::string* hostname_hash,
                                     std::vector<std::string>* slice_buffers,
                                     std::size_t index,
                                     std::uint64_t starting_index) {
    auto* pgraph = const_cast<internal::CallTree*>(merged)->get(key);
    if (pgraph) {
        internal::JsonSerializer serializer;
        char init[8];
        serializer.initialize(init, *hostname_hash);
        (void)init;
        serialize_process_slice(*pgraph, key, serializer, starting_index,
                                (*slice_buffers)[index]);
    }
    co_return;
}

coro::CoroTask<void> serialize_all_slices(
    CoroScope* child, const internal::CallTree* merged,
    const std::vector<internal::ProcessKey>* keys,
    const std::string* hostname_hash, std::vector<std::string>* slice_buffers,
    std::uint64_t stride) {
    for (std::size_t i = 0; i < keys->size(); ++i) {
        internal::ProcessKey k = (*keys)[i];
        std::uint64_t start_idx = i * stride;
        child->spawn([merged, k, start_idx, i, hostname_hash, slice_buffers](
                         CoroScope&) mutable -> coro::CoroTask<void> {
            co_await serialize_slice(merged, k, hostname_hash, slice_buffers, i,
                                     start_idx);
        });
    }
    co_return;
}

coro::CoroTask<void> task_write_json(RunCtx* ctx, CoroScope* scope) {
    if (ctx->failed || ctx->cli->no_save) co_return;
    const auto t0 = std::chrono::steady_clock::now();

    const std::size_t n = ctx->process_keys.size();
    std::vector<std::string> slice_buffers(n);
    static constexpr std::uint64_t IDX_STRIDE = 1ull << 20;

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname) - 1);
    std::string hostname_hash(hostname);

    std::vector<std::string>* slice_buffers_ptr = &slice_buffers;
    const std::string* hostname_hash_ptr = &hostname_hash;
    const internal::CallTree* merged = &ctx->merged;
    const std::vector<internal::ProcessKey>* keys_ptr = &ctx->process_keys;

    co_await scope->scope(
        [merged, keys_ptr, hostname_hash_ptr,
         slice_buffers_ptr](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await serialize_all_slices(&child, merged, keys_ptr,
                                          hostname_hash_ptr, slice_buffers_ptr,
                                          IDX_STRIDE);
        });

    std::string header;
    header.append("[\n", 2);
    {
        internal::JsonSerializer serializer;
        char init_buf[8];
        serializer.initialize(init_buf, hostname_hash);
        (void)init_buf;
        char buf[8192];
        std::time_t now = std::time(nullptr);
        char ts[64];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&now));
        std::size_t w = serializer.serialize_metadata(buf, "timestamp", ts, "M",
                                                      0, 0, true);
        if (w > 0) header.append(buf, w - 1);
        header.append(",\n", 2);
        w = serializer.serialize_metadata(buf, "format", "call_tree", "M", 0, 0,
                                          true);
        if (w > 0) header.append(buf, w - 1);
        header.append(",\n", 2);
    }

    fileio::parallel::WriterConfig wc;
    wc.layout = fileio::parallel::FileLayout::SHARDED;
    wc.gzip = ctx->cli->gzip;
    auto writer = fileio::parallel::make_writer(wc);

    const std::size_t total_workers = n + 1;
    if (co_await writer->open(ctx->output_path, total_workers, ctx->cli->gzip,
                              scope) != 0) {
        DFTRACER_UTILS_LOG_ERROR("failed to open writer: %s",
                                 ctx->output_path.c_str());
        ctx->failed = true;
        co_return;
    }

    if (co_await writer->write_chunk(
            0, ByteView(header.data(), header.size())) != 0) {
        ctx->failed = true;
    }

    for (std::size_t i = 0; i < n && !ctx->failed; ++i) {
        std::string& b = slice_buffers[i];
        if (i + 1 == n) {
            if (b.size() >= 2 && b[b.size() - 2] == ',' &&
                b[b.size() - 1] == '\n') {
                b.resize(b.size() - 2);
                b.append("\n]\n", 3);
            } else {
                b.append("]\n", 2);
            }
        }
        if (co_await writer->write_chunk(i + 1, ByteView(b.data(), b.size())) !=
            0) {
            ctx->failed = true;
            break;
        }
    }

    if (co_await writer->close() != 0) ctx->failed = true;

    if (!ctx->failed) {
        auto shards = writer->output_paths();
        if (co_await fileio::parallel::merge_shards(ctx->output_path, shards) !=
            0) {
            DFTRACER_UTILS_LOG_ERROR("merge_shards failed for %s",
                                     ctx->output_path.c_str());
            ctx->failed = true;
        }
    }

    ctx->write_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    DFTRACER_UTILS_LOG_DEBUG("[write] %.2f ms -> %s", ctx->write_ms,
                             ctx->output_path.c_str());
    co_return;
}

int run(int argc, char** argv) {
    dftracer::utils::logger::init();

    argparse::ArgumentParser program("dftracer_call_tree",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Build a call tree from DFTracer trace files and emit Chrome Tracing "
        "JSON.");

    CallTreeArgParse cli(program);
    if (!cli::setup_and_parse(cli, argc, argv)) return 1;

    RunCtx ctx;
    ctx.cli = &cli;

    if (cli.output.empty()) {
        std::string base = "call_tree";
        if (!cli.inputs.empty()) {
            fs::path p(cli.inputs.front());
            if (fs::is_directory(p))
                base = p.filename().string();
            else
                base = p.stem().string();
            if (base.empty()) base = "call_tree";
        }
        ctx.output_path = base + ".pfw";
    } else {
        ctx.output_path = cli.output;
    }
    if (cli.gzip &&
        (ctx.output_path.size() < 3 ||
         ctx.output_path.compare(ctx.output_path.size() - 3, 3, ".gz") != 0)) {
        ctx.output_path += ".gz";
    }

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer CallTree", cli.pipeline);
    Pipeline pipeline(pipeline_config);

    RunCtx* ctx_ptr = &ctx;
    auto scan = make_task(
        [ctx_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_scan(ctx_ptr, scope);
        },
        "scan");
    auto build = make_task(
        [ctx_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_build(ctx_ptr, &scope);
        },
        "build");
    auto merge = make_task(
        [ctx_ptr](CoroScope&) -> coro::CoroTask<void> {
            co_await task_merge(ctx_ptr);
        },
        "merge");
    auto hierarchy = make_task(
        [ctx_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_hierarchy(ctx_ptr, &scope);
        },
        "hierarchy");
    auto write = make_task(
        [ctx_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_write_json(ctx_ptr, &scope);
        },
        "write_json");

    build->depends_on(scan);
    merge->depends_on(build);
    hierarchy->depends_on(merge);
    write->depends_on(hierarchy);

    pipeline.set_source(scan);
    pipeline.set_destination(write);
    pipeline.execute();

    if (!ctx.failed) {
        DFTRACER_UTILS_LOG_DEBUG(
            "[done] scan=%.1fms build=%.1fms merge=%.1fms hierarchy=%.1fms "
            "write=%.1fms",
            ctx.scan_ms, ctx.build_ms, ctx.merge_ms, ctx.hier_ms, ctx.write_ms);
    }

    return ctx.failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) { return run(argc, argv); }
