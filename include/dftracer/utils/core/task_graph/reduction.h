#ifndef DFTRACER_UTILS_CORE_TASK_GRAPH_REDUCTION_H
#define DFTRACER_UTILS_CORE_TASK_GRAPH_REDUCTION_H

#include <dftracer/utils/core/task_graph/types.h>

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace dftracer::utils::task_graph {

/**
 * Partition items into groups of size n (Dask-style)
 *
 * The last group may be smaller if items.size() is not divisible by n.
 * This follows Dask's partition_all semantics.
 *
 * Examples:
 *   partition_all(2, [0,1,2,3,4,5,6]) -> [[0,1], [2,3], [4,5], [6]]
 *   partition_all(3, [0,1,2,3,4])     -> [[0,1,2], [3,4]]
 *   partition_all(4, [0,1,2])         -> [[0,1,2]]
 *
 * @param n Group size
 * @param items Items to partition
 * @return Vector of groups
 */
template <typename T>
std::vector<std::vector<T>> partition_all(std::size_t n,
                                          const std::vector<T>& items) {
    if (n == 0) {
        throw std::invalid_argument("partition_all: n must be > 0");
    }

    std::vector<std::vector<T>> result;
    if (items.empty()) {
        return result;
    }

    result.reserve((items.size() + n - 1) / n);

    for (std::size_t i = 0; i < items.size(); i += n) {
        std::size_t group_size = std::min(n, items.size() - i);
        std::vector<T> group;
        group.reserve(group_size);
        for (std::size_t j = 0; j < group_size; ++j) {
            group.push_back(items[i + j]);
        }
        result.push_back(std::move(group));
    }

    return result;
}

/**
 * Partition items by moving them (for non-copyable types)
 */
template <typename T>
std::vector<std::vector<T>> partition_all_move(std::size_t n,
                                               std::vector<T>& items) {
    if (n == 0) {
        throw std::invalid_argument("partition_all_move: n must be > 0");
    }

    std::vector<std::vector<T>> result;
    if (items.empty()) {
        return result;
    }

    result.reserve((items.size() + n - 1) / n);

    for (std::size_t i = 0; i < items.size(); i += n) {
        std::size_t group_size = std::min(n, items.size() - i);
        std::vector<T> group;
        group.reserve(group_size);
        for (std::size_t j = 0; j < group_size; ++j) {
            group.push_back(std::move(items[i + j]));
        }
        result.push_back(std::move(group));
    }

    return result;
}

/**
 * Calculate tree reduction depth for given number of items and split size
 *
 * @param num_items Number of items to reduce
 * @param split_size How many items to combine at each level
 * @return Number of reduction levels (0 if num_items <= 1)
 */
inline std::size_t tree_reduction_depth(std::size_t num_items,
                                        std::size_t split_size) {
    if (num_items <= 1 || split_size <= 1) {
        return 0;
    }

    std::size_t depth = 0;
    std::size_t current = num_items;
    while (current > 1) {
        current = (current + split_size - 1) / split_size;
        ++depth;
    }
    return depth;
}

/**
 * Calculate number of reduction tasks at each level
 *
 * Example with 7 items, split_every{2}:
 *   Level 0 (input):  7 items
 *   Level 1:          4 tasks (ceil(7/2) = 4, but one is pass-through)
 *   Level 2:          2 tasks
 *   Level 3:          1 task (final)
 *
 * @param num_items Starting number of items
 * @param split_size How many items to combine at each level
 * @return Vector of task counts per level (not including input level)
 */
inline std::vector<std::size_t> tree_reduction_levels(std::size_t num_items,
                                                      std::size_t split_size) {
    std::vector<std::size_t> levels;
    if (num_items <= 1 || split_size <= 1) {
        return levels;
    }

    std::size_t current = num_items;
    while (current > 1) {
        current = (current + split_size - 1) / split_size;
        levels.push_back(current);
    }
    return levels;
}

/**
 * Build tree reduction structure
 *
 * Returns indices for each level showing which items from the previous level
 * should be combined. Handles odd numbers by passing through singletons.
 *
 * Example: build_tree_indices(7, 2) returns:
 *   Level 1: [[0,1], [2,3], [4,5], [6]]      - 4 groups (last is singleton)
 *   Level 2: [[0,1], [2,3]]                   - 2 groups
 *   Level 3: [[0,1]]                          - 1 group (final)
 *
 * @param num_items Starting number of items
 * @param split_size How many items to combine at each level
 * @return Vector of levels, each containing groups of indices
 */
inline std::vector<std::vector<std::vector<std::size_t>>> build_tree_indices(
    std::size_t num_items, std::size_t split_size) {
    std::vector<std::vector<std::vector<std::size_t>>> levels;

    if (num_items <= 1 || split_size <= 1) {
        return levels;
    }

    // Create initial indices
    std::vector<std::size_t> current_indices;
    current_indices.reserve(num_items);
    for (std::size_t i = 0; i < num_items; ++i) {
        current_indices.push_back(i);
    }

    while (current_indices.size() > 1) {
        auto groups = partition_all(split_size, current_indices);
        levels.push_back(groups);

        // Next level indices are 0..groups.size()-1
        current_indices.clear();
        current_indices.reserve(groups.size());
        for (std::size_t i = 0; i < groups.size(); ++i) {
            current_indices.push_back(i);
        }
    }

    return levels;
}

}  // namespace dftracer::utils::task_graph

#endif  // DFTRACER_UTILS_CORE_TASK_GRAPH_REDUCTION_H
