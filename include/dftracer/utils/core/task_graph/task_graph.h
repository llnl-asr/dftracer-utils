#ifndef DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GRAPH_H
#define DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GRAPH_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/task_graph/reduction.h>
#include <dftracer/utils/core/task_graph/task_graph_config.h>
#include <dftracer/utils/core/task_graph/task_group.h>
#include <dftracer/utils/core/task_graph/task_result.h>
#include <dftracer/utils/core/task_graph/types.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::task_graph {

// ============================================================================
// Factory Functions
// ============================================================================

/**
 * Create fan-out tasks: 1 source -> N outputs
 *
 * @param source Source task whose output will be fanned out
 * @param count Number of output tasks to create
 * @param mapper Function to create each output task
 * @param name_prefix Prefix for task names (tasks will be named prefix_0,
 * prefix_1, etc.)
 * @return Vector of created tasks
 */
template <typename Func>
std::vector<std::shared_ptr<Task>> make_fan_out(
    std::shared_ptr<Task> source, num_outputs count, Func&& mapper,
    std::string name_prefix = "FanOut") {
    std::vector<std::shared_ptr<Task>> outputs;
    outputs.reserve(count.count);

    for (std::size_t i = 0; i < count.count; ++i) {
        auto task_name = name_prefix + "_" + std::to_string(i);
        // Create task that calls mapper with index
        auto task = make_task(
            [mapper = std::forward<Func>(mapper), i](CoroScope& ctx, auto input)
                -> decltype(mapper(ctx, std::declval<decltype(input)>(), i)) {
                return mapper(ctx, std::move(input), i);
            },
            task_name);
        task->depends_on(source);
        outputs.push_back(std::move(task));
    }

    return outputs;
}

/**
 * Create fan-in task: M sources -> 1 output
 *
 * @param sources Vector of source tasks to combine
 * @param combiner Function that combines all outputs into one
 * @param name Task name
 * @return The combined task
 */
template <typename Combiner>
std::shared_ptr<Task> make_fan_in(std::vector<std::shared_ptr<Task>> sources,
                                  Combiner&& combiner,
                                  std::string name = "FanIn") {
    auto task = make_task(std::forward<Combiner>(combiner), name);

    // Set up dependencies
    for (auto& source : sources) {
        task->depends_on(source);
    }

    return task;
}

/**
 * Create tree reduction: M sources -> 1 output via O(log N) tree
 *
 * Uses Dask-style odd handling: singletons pass through to next level.
 *
 * @param sources Vector of source tasks to reduce
 * @param split How many tasks to combine at each level
 * @param reducer Binary function that combines two items
 * @param name_prefix Prefix for reduction task names
 * @return The final reduced task
 */
template <typename Reducer>
std::shared_ptr<Task> make_tree_reduce(
    std::vector<std::shared_ptr<Task>> sources, split_every split,
    Reducer&& reducer, std::string name_prefix = "Reduce") {
    if (sources.empty()) {
        throw std::invalid_argument(
            "make_tree_reduce: sources cannot be empty");
    }

    if (sources.size() == 1) {
        return sources[0];
    }

    if (split.count < 2) {
        throw std::invalid_argument(
            "make_tree_reduce: split_every must be >= 2");
    }

    std::vector<std::shared_ptr<Task>> current_level = std::move(sources);
    std::size_t level = 0;

    while (current_level.size() > 1) {
        auto groups = partition_all(split.count, current_level);
        std::vector<std::shared_ptr<Task>> next_level;
        next_level.reserve(groups.size());

        for (std::size_t group_idx = 0; group_idx < groups.size();
             ++group_idx) {
            auto& group = groups[group_idx];

            if (group.size() == 1) {
                // Singleton: pass through to next level
                next_level.push_back(std::move(group[0]));
            } else {
                // Multiple items: create reduction task
                auto task_name = name_prefix + "_L" + std::to_string(level) +
                                 "_G" + std::to_string(group_idx);

                // Create task that reduces the group
                auto reduction_task =
                    make_task(std::forward<Reducer>(reducer), task_name);

                // Set up dependencies on all tasks in the group
                for (auto& task : group) {
                    reduction_task->depends_on(task);
                }

                next_level.push_back(std::move(reduction_task));
            }
        }

        current_level = std::move(next_level);
        ++level;
    }

    return current_level[0];
}

// ============================================================================
// TaskGraph Builder
// ============================================================================

/**
 * @brief Builder for constructing task DAGs.
 *
 * Features:
 * - Fluent API for building task graphs
 * - Optional auto-registration with Pipeline
 * - Support for fan-in, fan-out, transform, reduce patterns
 * - Bi-directional connectivity with external tasks via wrap()
 * - Configurable max_concurrency to limit in-flight parallel tasks
 *
 * Usage:
 * @code
 *   Pipeline pipeline(config);
 *   auto graph = TaskGraph::builder({.name = "MyGraph",
 *                                    .max_concurrency = 128});
 *   auto readers = graph.parallel<int>(8, reader_fn,
 *                                      {.name = "Reader"});
 *   auto reduced = graph.reduce<int>(readers, split_every{2}, reducer,
 *                                    {.name = "Merge"});
 *   pipeline.set_source(readers.tasks()[0]);
 *   pipeline.execute();
 * @endcode
 */
class TaskGraph {
   public:
    /**
     * Create a TaskGraph builder with options
     */
    static TaskGraph builder(TaskGraphConfig opts = {}) {
        return TaskGraph(std::move(opts));
    }

    /**
     * Wrap an external task as entry point to the graph
     *
     * Use this to connect TaskGraph operations to existing barebone tasks.
     */
    template <typename T>
    TaskGroup<T> wrap(std::shared_ptr<Task> external_task) {
        // Don't add to pipeline - it's already managed externally
        all_tasks_.push_back(external_task);
        return TaskGroup<T>(external_task);
    }

    /**
     * Add an external task to the graph
     *
     * Use this to add utility-adapter tasks or other externally created tasks
     * that should be tracked by the graph but weren't created via graph
     * methods.
     */
    void add(std::shared_ptr<Task> task) { all_tasks_.push_back(task); }

    /**
     * Create a single source task
     */
    template <typename T, typename Func>
    TaskGroup<T> source(Func&& func, TaskGraphSourceConfig opts = {}) {
        auto task = make_task(std::forward<Func>(func), opts.name);
        register_task(task);
        return TaskGroup<T>(task);
    }

    /**
     * Create N parallel tasks (no dependencies between them)
     *
     * Each task receives its index (0 to count-1) as parameter.
     *
     * When max_concurrency is set (via options or graph-level default),
     * a sliding window dependency chain limits in-flight tasks:
     * task[i] depends on task[i - max_concurrency], so at most
     * max_concurrency tasks execute simultaneously.
     */
    template <typename T, typename Func>
    TaskGroup<T> parallel(std::size_t count, Func&& func,
                          TaskGraphParallelConfig opts = {}) {
        auto max_conc = resolve_max_concurrency(opts.max_concurrency);

        TaskGroup<T> group;
        group.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            auto task_name = opts.name + "_" + std::to_string(i);
            // Wrap the function to pass the index
            auto task =
                make_task([func = func,
                           i](CoroScope& ctx) mutable { return func(ctx, i); },
                          task_name);

            // Sliding window: task[i] waits for task[i - W] to complete
            if (max_conc > 0 && i >= max_conc) {
                task->depends_on(group[i - max_conc]);
            }

            register_task(task);
            group.add(task);
        }

        return group;
    }

    /**
     * Fan-out: 1 -> N (one input produces N outputs)
     *
     * Each output task receives the source output and its index.
     *
     * When max_concurrency is set, a sliding window dependency chain
     * limits in-flight fan-out tasks.
     */
    template <typename U, typename T, typename Func>
    TaskGroup<U> fan_out(const TaskGroup<T>& source, num_outputs count,
                         Func&& mapper, TaskGraphFanOutConfig opts = {}) {
        if (source.size() != 1) {
            throw std::invalid_argument(
                "fan_out: source must have exactly 1 task");
        }

        auto max_conc = resolve_max_concurrency(opts.max_concurrency);

        TaskGroup<U> group;
        group.reserve(count.count);

        auto source_task = source.task();

        for (std::size_t i = 0; i < count.count; ++i) {
            auto task_name = opts.name + "_" + std::to_string(i);
            auto task = make_task(
                [mapper = mapper, i](CoroScope& ctx, T input) mutable {
                    return mapper(ctx, std::move(input), i);
                },
                task_name);
            task->depends_on(source_task);

            // Sliding window among fan-out siblings
            if (max_conc > 0 && i >= max_conc) {
                task->depends_on(group[i - max_conc]);
            }

            register_task(task);
            group.add(task);
        }

        return group;
    }

    /**
     * Fan-in: M -> 1 (all inputs combine to single output)
     */
    template <typename U, typename T, typename Combiner>
    TaskGroup<U> fan_in(const TaskGroup<T>& group, Combiner&& combiner,
                        TaskGraphFanInConfig opts = {}) {
        auto task = make_task(std::forward<Combiner>(combiner), opts.name);

        for (const auto& source : group.tasks()) {
            task->depends_on(source);
        }

        register_task(task);
        return TaskGroup<U>(task);
    }

    /**
     * Fan-in with grouping: M -> ceil(M/N) (group every N inputs)
     */
    template <typename U, typename T, typename Combiner>
    TaskGroup<U> fan_in(const TaskGroup<T>& group, split_every count,
                        Combiner&& combiner, TaskGraphFanInConfig opts = {}) {
        auto groups = partition_all(count.count, group.tasks());
        TaskGroup<U> result;
        result.reserve(groups.size());

        for (std::size_t i = 0; i < groups.size(); ++i) {
            auto& task_group = groups[i];
            auto task_name = opts.name + "_" + std::to_string(i);

            if (task_group.size() == 1) {
                // Singleton: might need type conversion or pass-through
                // For now, pass through
                result.add(task_group[0]);
            } else {
                auto task = make_task(combiner, task_name);
                for (auto& source : task_group) {
                    task->depends_on(source);
                }
                register_task(task);
                result.add(task);
            }
        }

        return result;
    }

    /**
     * Map: 1-to-1 mapping
     *
     * When max_concurrency is set, a sliding window dependency chain
     * limits in-flight map tasks.
     */
    template <typename U, typename T, typename Mapper>
    TaskGroup<U> map(const TaskGroup<T>& group, Mapper&& mapper,
                     TaskGraphMapConfig opts = {}) {
        auto max_conc = resolve_max_concurrency(opts.max_concurrency);

        TaskGroup<U> result;
        result.reserve(group.size());

        for (std::size_t i = 0; i < group.size(); ++i) {
            auto task_name = opts.name + "_" + std::to_string(i);
            auto task = make_task(std::forward<Mapper>(mapper), task_name);
            task->depends_on(group[i]);

            // Sliding window among map siblings
            if (max_conc > 0 && i >= max_conc) {
                task->depends_on(result[i - max_conc]);
            }

            register_task(task);
            result.add(task);
        }

        return result;
    }

    /**
     * Tree reduce: M -> 1 with O(log N) depth
     */
    template <typename U, typename T, typename Reducer>
    TaskGroup<U> reduce(const TaskGroup<T>& group, split_every count,
                        Reducer&& reducer, TaskGraphReduceConfig opts = {}) {
        if (group.empty()) {
            throw std::invalid_argument("reduce: group cannot be empty");
        }

        if (group.size() == 1) {
            auto task_name = opts.name + "_L0_G0";
            auto task = make_task(reducer, task_name);
            task->depends_on(group.task());
            // Wrap the single parent output into vector<any> so decode_input
            // can unpack it into vector<T> — same format as the multi-parent
            // path. Without this, the type checker rejects T -> vector<T>.
            task->with_combiner(
                [](const std::vector<std::any>& inputs) -> std::any {
                    return std::make_any<std::vector<std::any>>(inputs);
                });
            register_task(task);
            return TaskGroup<U>(task);
        }

        std::vector<std::shared_ptr<Task>> current_level = group.tasks();
        std::size_t level = 0;

        while (current_level.size() > 1) {
            auto groups = partition_all(count.count, current_level);
            std::vector<std::shared_ptr<Task>> next_level;
            next_level.reserve(groups.size());

            for (std::size_t group_idx = 0; group_idx < groups.size();
                 ++group_idx) {
                auto& task_group = groups[group_idx];

                if (task_group.size() == 1) {
                    // Singleton: pass through
                    next_level.push_back(std::move(task_group[0]));
                } else {
                    auto task_name = opts.name + "_L" + std::to_string(level) +
                                     "_G" + std::to_string(group_idx);
                    auto task = make_task(reducer, task_name);

                    for (auto& source : task_group) {
                        task->depends_on(source);
                    }

                    register_task(task);
                    next_level.push_back(std::move(task));
                }
            }

            current_level = std::move(next_level);
            ++level;
        }

        return TaskGroup<U>(current_level[0]);
    }

    /**
     * Fold: tree reduction with initial value and binary operation
     *
     * Each node folds its inputs: op(op(op(init, a), b), c)
     */
    template <typename T, typename BinaryOp>
    TaskGroup<T> fold(const TaskGroup<T>& group, T init, split_every count,
                      BinaryOp&& op, TaskGraphFoldConfig opts = {}) {
        if (group.empty()) {
            throw std::invalid_argument("fold: group cannot be empty");
        }

        if (group.size() == 1) {
            return TaskGroup<T>(group.task());
        }

        std::vector<std::shared_ptr<Task>> current_level = group.tasks();
        std::size_t level = 0;

        while (current_level.size() > 1) {
            auto groups = partition_all(count.count, current_level);
            std::vector<std::shared_ptr<Task>> next_level;
            next_level.reserve(groups.size());

            for (std::size_t group_idx = 0; group_idx < groups.size();
                 ++group_idx) {
                auto& task_group = groups[group_idx];

                if (task_group.size() == 1) {
                    next_level.push_back(std::move(task_group[0]));
                } else {
                    auto task_name = opts.name + "_L" + std::to_string(level) +
                                     "_G" + std::to_string(group_idx);
                    auto task = make_task(
                        [op = op, init](CoroScope&, std::vector<T> items)
                            -> coro::CoroTask<T> {
                            T acc = init;
                            for (const auto& item : items) {
                                acc = op(acc, item);
                            }
                            co_return acc;
                        },
                        task_name);

                    for (auto& source : task_group) {
                        task->depends_on(source);
                    }

                    register_task(task);
                    next_level.push_back(std::move(task));
                }
            }

            current_level = std::move(next_level);
            ++level;
        }

        return TaskGroup<T>(current_level[0]);
    }

    /**
     * Aggregate: map-reduce pattern
     *
     * Applies mapper to each task (1:1), then reduces the results.
     */
    template <typename U, typename Intermediate, typename T, typename MapFn,
              typename ReduceFn>
    TaskGroup<U> aggregate(const TaskGroup<T>& group, MapFn&& map_fn,
                           split_every count, ReduceFn&& reduce_fn,
                           TaskGraphAggregateConfig opts = {}) {
        auto mapped = map<Intermediate>(group, std::forward<MapFn>(map_fn),
                                        {.name = opts.name + "_Map"});
        return reduce<U>(mapped, count, std::forward<ReduceFn>(reduce_fn),
                         {.name = opts.name + "_Reduce"});
    }

    /**
     * Partition: split data into N contiguous chunks
     *
     * Each task produces its chunk as std::vector<T>.
     */
    template <typename T>
    TaskGroup<std::vector<T>> partition(const std::vector<T>& data,
                                        num_partitions count,
                                        TaskGraphPartitionConfig opts = {}) {
        if (count.count == 0) {
            throw std::invalid_argument("partition: count must be > 0");
        }

        TaskGroup<std::vector<T>> group;
        group.reserve(count.count);

        std::size_t total = data.size();
        std::size_t base_size = total / count.count;
        std::size_t remainder = total % count.count;

        std::size_t start = 0;
        for (std::size_t i = 0; i < count.count; ++i) {
            std::size_t chunk_size = base_size + (i < remainder ? 1 : 0);
            std::size_t end = start + chunk_size;

            auto task_name = opts.name + "_" + std::to_string(i);
            std::vector<T> chunk(data.begin() + start, data.begin() + end);

            auto task = make_task(
                [chunk = std::move(chunk)](CoroScope&)
                    -> coro::CoroTask<std::vector<T>> { co_return chunk; },
                task_name);

            register_task(task);
            group.add(task);
            start = end;
        }

        return group;
    }

    /**
     * Concat partitions: combine vector partitions into a single vector
     *
     * Uses tree reduction to efficiently concatenate.
     */
    template <typename T>
    TaskGroup<std::vector<T>> concat_partitions(
        const TaskGroup<std::vector<T>>& group,
        split_every count = split_every{2}, TaskGraphConcatConfig opts = {}) {
        if (group.empty()) {
            throw std::invalid_argument(
                "concat_partitions: group cannot be empty");
        }

        if (group.size() == 1) {
            return TaskGroup<std::vector<T>>(group.task());
        }

        std::vector<std::shared_ptr<Task>> current_level = group.tasks();
        std::size_t level = 0;

        while (current_level.size() > 1) {
            auto groups = partition_all(count.count, current_level);
            std::vector<std::shared_ptr<Task>> next_level;
            next_level.reserve(groups.size());

            for (std::size_t group_idx = 0; group_idx < groups.size();
                 ++group_idx) {
                auto& task_group = groups[group_idx];

                if (task_group.size() == 1) {
                    next_level.push_back(std::move(task_group[0]));
                } else {
                    auto task_name = opts.name + "_L" + std::to_string(level) +
                                     "_G" + std::to_string(group_idx);
                    auto task = make_task(
                        [](CoroScope&, std::vector<std::vector<T>> chunks)
                            -> coro::CoroTask<std::vector<T>> {
                            std::vector<T> result;
                            for (auto& chunk : chunks) {
                                result.insert(result.end(), chunk.begin(),
                                              chunk.end());
                            }
                            co_return result;
                        },
                        task_name);

                    for (auto& source : task_group) {
                        task->depends_on(source);
                    }

                    register_task(task);
                    next_level.push_back(std::move(task));
                }
            }

            current_level = std::move(next_level);
            ++level;
        }

        return TaskGroup<std::vector<T>>(current_level[0]);
    }

    /**
     * Get first task in the graph (convenience for setting Pipeline source)
     */
    std::shared_ptr<Task> first_task() const {
        if (all_tasks_.empty()) {
            return nullptr;
        }
        return all_tasks_.front();
    }

    /**
     * Get last task in the graph (convenience for setting Pipeline destination)
     */
    std::shared_ptr<Task> last_task() const {
        if (all_tasks_.empty()) {
            return nullptr;
        }
        return all_tasks_.back();
    }

    /**
     * Get all tasks in the graph
     */
    const std::vector<std::shared_ptr<Task>>& tasks() const {
        return all_tasks_;
    }

    /**
     * Get graph name
     */
    const std::string& name() const { return name_; }

   private:
    explicit TaskGraph(TaskGraphConfig opts)
        : name_(std::move(opts.name)), max_concurrency_(opts.max_concurrency) {}

    /**
     * Resolve effective max_concurrency: per-method override wins,
     * then graph-level default, 0 = unlimited.
     */
    std::size_t resolve_max_concurrency(std::size_t method_override) const {
        if (method_override > 0) {
            return method_override;
        }
        return max_concurrency_;
    }

    void register_task(std::shared_ptr<Task> task) {
        all_tasks_.push_back(task);
    }

    std::string name_;
    std::size_t max_concurrency_ = 0;
    std::vector<std::shared_ptr<Task>> all_tasks_;
};

}  // namespace dftracer::utils::task_graph

#endif  // DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GRAPH_H
