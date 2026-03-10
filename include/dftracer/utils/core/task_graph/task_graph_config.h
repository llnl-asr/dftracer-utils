#ifndef DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GRAPH_CONFIG_H
#define DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GRAPH_CONFIG_H

#include <cstddef>
#include <string>

namespace dftracer::utils::task_graph {

/**
 * Config for TaskGraph builder.
 *
 * max_concurrency controls how many parallel/fan-out/map tasks can be
 * in-flight simultaneously.  0 = unlimited (all tasks start as soon as
 * their dependencies are met).  When set, individual method configs can
 * override this default.
 */
struct TaskGraphConfig {
    std::string name;
    std::size_t max_concurrency = 0;
};

/**
 * Config for TaskGraph::source()
 */
struct TaskGraphSourceConfig {
    std::string name = "Source";
};

/**
 * Config for TaskGraph::parallel()
 *
 * max_concurrency overrides the graph-level default when non-zero.
 */
struct TaskGraphParallelConfig {
    std::string name = "Parallel";
    std::size_t max_concurrency = 0;
};

/**
 * Config for TaskGraph::fan_out()
 *
 * max_concurrency overrides the graph-level default when non-zero.
 */
struct TaskGraphFanOutConfig {
    std::string name = "FanOut";
    std::size_t max_concurrency = 0;
};

/**
 * Config for TaskGraph::fan_in() (single and grouped overloads)
 */
struct TaskGraphFanInConfig {
    std::string name = "FanIn";
};

/**
 * Config for TaskGraph::map()
 *
 * max_concurrency overrides the graph-level default when non-zero.
 */
struct TaskGraphMapConfig {
    std::string name = "Map";
    std::size_t max_concurrency = 0;
};

/**
 * Config for TaskGraph::reduce()
 */
struct TaskGraphReduceConfig {
    std::string name = "Reduce";
};

/**
 * Config for TaskGraph::fold()
 */
struct TaskGraphFoldConfig {
    std::string name = "Fold";
};

/**
 * Config for TaskGraph::aggregate()
 */
struct TaskGraphAggregateConfig {
    std::string name = "Aggregate";
};

/**
 * Config for TaskGraph::partition()
 */
struct TaskGraphPartitionConfig {
    std::string name = "Partition";
};

/**
 * Config for TaskGraph::concat_partitions()
 */
struct TaskGraphConcatConfig {
    std::string name = "Concat";
};

}  // namespace dftracer::utils::task_graph

#endif  // DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GRAPH_CONFIG_H
