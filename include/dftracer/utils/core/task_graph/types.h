#ifndef DFTRACER_UTILS_CORE_TASK_GRAPH_TYPES_H
#define DFTRACER_UTILS_CORE_TASK_GRAPH_TYPES_H

#include <cstddef>

namespace dftracer::utils::task_graph {

/**
 * Strong type for fan-in operations: group every N inputs together
 *
 * Used with fan_in() and reduce() to specify how many inputs to combine
 * at each step. Follows Dask's split_every semantics.
 *
 * Example:
 *   split_every{2} with 7 inputs:
 *     Level 0: [0,1,2,3,4,5,6]
 *     Level 1: [reduce(0,1), reduce(2,3), reduce(4,5), 6]  -> 4 items
 *     Level 2: [reduce(01,23), reduce(45,6)]               -> 2 items
 *     Level 3: [reduce(0123,456)]                          -> 1 item
 */
struct split_every {
    std::size_t count;

    constexpr explicit split_every(std::size_t n) : count(n) {}
};

/**
 * Strong type for fan-out operations: produce N outputs from one input
 *
 * Used with fan_out() to specify how many output tasks to create
 * from a single input task.
 *
 * Example:
 *   num_outputs{8} creates 8 parallel tasks from one source
 */
struct num_outputs {
    std::size_t count;

    constexpr explicit num_outputs(std::size_t n) : count(n) {}
};

/**
 * Strong type for partition operations: split data into N chunks
 *
 * Used with partition() to specify how many partitions to create
 * from input data. Each partition receives a contiguous chunk.
 *
 * Example:
 *   num_partitions{4} splits [1,2,3,4,5,6,7,8] into:
 *     Partition 0: [1,2]
 *     Partition 1: [3,4]
 *     Partition 2: [5,6]
 *     Partition 3: [7,8]
 */
struct num_partitions {
    std::size_t count;

    constexpr explicit num_partitions(std::size_t n) : count(n) {}
};

}  // namespace dftracer::utils::task_graph

#endif  // DFTRACER_UTILS_CORE_TASK_GRAPH_TYPES_H
