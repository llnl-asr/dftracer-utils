// MPI driver for parallel call-tree construction. Thin DAG over
// MPICallTreeBuilder; all phase logic lives in the engine.

#include <dftracer/utils/call_tree/mpi/builder.h>
#include <dftracer/utils/call_tree/mpi/config.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <mpi.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::call_tree;

namespace {

class CallTreeMpiArgParse : public cli::ArgParse {
   public:
    cli::PipelineArgs pipeline;

    std::string input_dir;
    std::string output;
    std::string staging_dir;
    bool verbose = false;
    bool gzip = false;
    bool keep_staging = false;

    explicit CallTreeMpiArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(pipeline);
    }

   protected:
    void register_args() override {
        parser().add_argument("input").help(
            "Input directory containing trace files");
        parser()
            .add_argument("-o", "--output")
            .help("Output JSON path")
            .default_value<std::string>("call_tree.pfw");
        parser()
            .add_argument("--staging-dir")
            .help(
                "Shared FS staging root for per-rank shards (default "
                "<output>.shards/)")
            .default_value<std::string>("");
        parser().add_argument("--gzip").flag();
        parser().add_argument("-v", "--verbose").flag();
        parser().add_argument("--keep-staging").flag();
    }

    void post_parse() override {
        input_dir = parser().get<std::string>("input");
        output = parser().get<std::string>("--output");
        staging_dir = parser().get<std::string>("--staging-dir");
        gzip = parser().get<bool>("--gzip");
        verbose = parser().get<bool>("--verbose");
        keep_staging = parser().get<bool>("--keep-staging");
    }
};

struct RunCtx {
    const CallTreeMpiArgParse* cli = nullptr;
    std::unique_ptr<MPICallTreeBuilder> builder;
    std::string final_output;
    std::string staging_dir;
    bool failed = false;
};

int run(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_call_tree_mpi",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "MPI driver for parallel call-tree construction. Each rank owns a "
        "slice of PIDs and emits a Chrome Tracing JSON shard; rank 0 merges.");

    CallTreeMpiArgParse cli(program);
    cli.setup();
    if (!cli.parse(argc, argv)) return 1;

    int rank = 0, size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Scale per-rank threads down when multiple ranks share a node.
    MPI_Comm node_comm = MPI_COMM_NULL;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank,
                        MPI_INFO_NULL, &node_comm);
    int ppn = 1;
    if (node_comm != MPI_COMM_NULL) {
        MPI_Comm_size(node_comm, &ppn);
        MPI_Comm_free(&node_comm);
    }
    if (ppn > 1) {
        const auto hw = dftracer_utils_hardware_concurrency();
        const auto scaled = std::max<std::size_t>(
            1, static_cast<std::size_t>(hw) / static_cast<std::size_t>(ppn));
        if (cli.pipeline.executor_threads == static_cast<std::size_t>(hw))
            cli.pipeline.executor_threads = scaled;
        if (cli.pipeline.io_threads == static_cast<std::size_t>(hw))
            cli.pipeline.io_threads = scaled;
    }

    RunCtx ctx;
    ctx.cli = &cli;

    MPICallTreeConfig builder_cfg;
    builder_cfg.verbose = cli.verbose;
    ctx.builder = std::make_unique<MPICallTreeBuilder>(builder_cfg);

    ctx.final_output = fs::absolute(cli.output).string();
    if (cli.gzip && (ctx.final_output.size() < 3 ||
                     ctx.final_output.compare(ctx.final_output.size() - 3, 3,
                                              ".gz") != 0)) {
        ctx.final_output += ".gz";
    }
    ctx.staging_dir = cli.staging_dir.empty()
                          ? (ctx.final_output + ".shards")
                          : fs::absolute(cli.staging_dir).string();

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer CallTree MPI", cli.pipeline);
    Pipeline pipeline(pipeline_config);

    RunCtx* p = &ctx;
    auto discover = make_task(
        [p](CoroScope& scope) -> coro::CoroTask<void> {
            if (p->failed) co_return;
            p->builder->add_trace_directory(p->cli->input_dir);
            if (p->builder->trace_files().empty()) {
                if (p->builder->rank() == 0)
                    std::fprintf(stderr, "no .pfw/.pfw.gz files in %s\n",
                                 p->cli->input_dir.c_str());
                p->failed = true;
                co_return;
            }
            if (!co_await p->builder->discover_pids(&scope)) p->failed = true;
        },
        "discover");
    auto build = make_task(
        [p](CoroScope& scope) -> coro::CoroTask<void> {
            if (p->failed) co_return;
            if (!co_await p->builder->build(&scope)) p->failed = true;
        },
        "build");
    auto hierarchy = make_task(
        [p](CoroScope& scope) -> coro::CoroTask<void> {
            if (p->failed) co_return;
            if (!co_await p->builder->hierarchy(&scope)) p->failed = true;
        },
        "hierarchy");
    auto write = make_task(
        [p](CoroScope& scope) -> coro::CoroTask<void> {
            if (p->failed) co_return;
            if (!co_await p->builder->write(&scope, p->final_output,
                                            p->staging_dir, p->cli->gzip))
                p->failed = true;
        },
        "write");
    auto merge = make_task(
        [p](CoroScope&) -> coro::CoroTask<void> {
            if (p->failed) co_return;
            if (!co_await p->builder->merge(p->final_output, p->staging_dir,
                                            p->cli->gzip, p->cli->keep_staging))
                p->failed = true;
        },
        "merge");

    build->depends_on(discover);
    hierarchy->depends_on(build);
    write->depends_on(hierarchy);
    merge->depends_on(write);

    pipeline.set_source(discover);
    pipeline.set_destination(merge);
    pipeline.execute();

    MPI_Barrier(MPI_COMM_WORLD);
    return ctx.failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_FUNNELED) {
        std::fprintf(stderr,
                     "MPI does not support MPI_THREAD_FUNNELED (got %d), "
                     "aborting\n",
                     provided);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    const int rc = run(argc, argv);
    MPI_Finalize();
    return rc;
}
