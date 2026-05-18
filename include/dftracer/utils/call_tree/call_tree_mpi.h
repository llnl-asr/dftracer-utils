#ifndef DFTRACER_UTILS_CALL_TREE_MPI_H
#define DFTRACER_UTILS_CALL_TREE_MPI_H

// MPI call-tree umbrella header. The engine is the coroutine-driven
// MPICallTreeBuilder; older per-component headers (serializable,
// file_header, build_task, filtered_reader, pid_index_info, serialization)
// were removed when the build/gather phases moved to ParallelWriter +
// merge_shards on Chrome Tracing JSON output.

#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/mpi/builder.h>
#include <dftracer/utils/call_tree/mpi/config.h>

#endif  // DFTRACER_UTILS_CALL_TREE_MPI_H
