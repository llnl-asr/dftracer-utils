#ifndef DFTRACER_UTILS_BINARIES_COMMON_CLI_MPI_H
#define DFTRACER_UTILS_BINARIES_COMMON_CLI_MPI_H

#include <mpi.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include "common_cli.h"

namespace dftracer::utils::cli {

// Standard MPI entry point: init with THREAD_MULTIPLE, run, finalize.
// Aborts if the implementation cannot provide at least THREAD_FUNNELED.
inline int mpi_main(int argc, char** argv, int (*run)(int, char**)) {
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

// When multiple ranks share a node, divide executor/io threads by the per-node
// rank count so the ranks don't collectively oversubscribe the cores. Only
// values still at the argparse default (hardware_concurrency()) are scaled;
// explicit user-provided values are left alone. With verbose, rank 0 prints the
// detected layout.
inline void scale_threads_for_ppn(PipelineArgs& pipeline, int rank,
                                  bool verbose = false) {
    MPI_Comm node_comm = MPI_COMM_NULL;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank,
                        MPI_INFO_NULL, &node_comm);
    int ppn = 1;
    if (node_comm != MPI_COMM_NULL) {
        MPI_Comm_size(node_comm, &ppn);
        MPI_Comm_free(&node_comm);
    }
    if (ppn <= 1) return;

    const auto hw = dftracer_utils_hardware_concurrency();
    const auto scaled = std::max<std::size_t>(
        1, static_cast<std::size_t>(hw) / static_cast<std::size_t>(ppn));
    if (pipeline.executor_threads == static_cast<std::size_t>(hw)) {
        pipeline.executor_threads = scaled;
    }
    if (pipeline.io_threads == static_cast<std::size_t>(hw)) {
        pipeline.io_threads = scaled;
    }
    if (verbose && rank == 0) {
        std::printf(
            "[rank 0] detected ppn=%d, executor_threads=%zu io_threads=%zu "
            "(hw=%zu)\n",
            ppn, pipeline.executor_threads, pipeline.io_threads,
            static_cast<std::size_t>(hw));
        std::fflush(stdout);
    }
}

}  // namespace dftracer::utils::cli

#endif  // DFTRACER_UTILS_BINARIES_COMMON_CLI_MPI_H
