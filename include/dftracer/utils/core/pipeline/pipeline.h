#ifndef DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H
#define DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/pipeline/pipeline_output.h>
#include <dftracer/utils/core/pipeline/scheduler.h>

#include <any>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils {

class Task;
class Executor;

/**
 * Pipeline - DAG container and orchestrator
 *
 * Features:
 * - Holds source and destination tasks
 * - Validates DAG before execution (reachability, types, cycles)
 * - Delegates execution to scheduler/executor
 * - Automatically creates NoOpTask for multiple sources
 */
class Pipeline {
   private:
    std::shared_ptr<Task> source_;       // Single source (may be NoOpTask)
    std::shared_ptr<Task> destination_;  // Can be nullptr

    std::vector<std::shared_ptr<Task>> all_tasks_;  // All reachable tasks

    std::unique_ptr<Executor> executor_;
    std::unique_ptr<Scheduler> scheduler_;

    std::string name_;
    bool validated_{false};

    size_t executor_threads_;
    ErrorPolicy error_policy_{ErrorPolicy::FAIL_FAST};
    ErrorHandler error_handler_{nullptr};

   public:
    /**
     * Constructor with configuration manager
     * @param config Pipeline configuration (includes name, threads, etc.)
     */
    explicit Pipeline(const PipelineConfigManager& config =
                          PipelineConfigManager::default_config());

    ~Pipeline();

    // Prevent copying
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Allow moving
    Pipeline(Pipeline&&) = default;
    Pipeline& operator=(Pipeline&&) = default;

    /**
     * Set source task
     */
    void set_source(std::shared_ptr<Task> source);

    /**
     * Set destination task (optional - if nullptr, all terminal tasks are
     * destinations)
     */
    void set_destination(std::shared_ptr<Task> destination);

    /**
     * Add multiple source tasks (auto-creates NoOpTask as parent)
     */
    void add_sources(std::initializer_list<std::shared_ptr<Task>> sources);

    /**
     * Validate pipeline before execution
     * - Check reachability from source to destination
     * - Check type compatibility
     * - Check for cycles
     */
    bool validate();

    /**
     * Execute the pipeline
     * @param input Initial input for source task (defaults to empty)
     * @return Output from terminal tasks
     */
    PipelineOutput execute(const std::any& input = std::any{});

    /**
     * Execute the pipeline with typed input
     * @tparam T Input type
     * @param input Initial input for source task
     * @return Output from terminal tasks
     */
    template <typename T, typename = std::enable_if_t<
                              !std::is_same_v<std::decay_t<T>, std::any>>>
    PipelineOutput execute(T&& input) {
        return execute(std::any{std::forward<T>(input)});
    }

    /**
     * Set error handling policy
     */
    void set_error_policy(ErrorPolicy policy);

    /**
     * Set progress callback
     */
    void set_progress_callback(
        std::function<void(size_t completed, size_t total)> callback);

    /**
     * Get pipeline name
     */
    const std::string& get_name() const { return name_; }

    /**
     * Get source task
     */
    std::shared_ptr<Task> get_source() const { return source_; }

    /**
     * Get destination task
     */
    std::shared_ptr<Task> get_destination() const { return destination_; }

    /**
     * Get all tasks in the pipeline
     */
    const std::vector<std::shared_ptr<Task>>& get_all_tasks() const {
        return all_tasks_;
    }

   private:
    /**
     * Validate reachability from source to destination (if set)
     */
    bool validate_reachability();

    /**
     * Validate type compatibility across all edges
     */
    bool validate_types();

    /**
     * Check for cycles in the DAG
     */
    bool has_cycles();

    /**
     * Collect all reachable tasks from source
     */
    void collect_all_tasks();

    /**
     * DFS helper for collecting tasks
     */
    void collect_tasks_dfs(std::shared_ptr<Task> task,
                           std::unordered_set<TaskIndex>& visited);

    /**
     * DFS helper for cycle detection
     */
    bool has_cycles_dfs(std::shared_ptr<Task> task,
                        std::unordered_set<TaskIndex>& visited,
                        std::unordered_set<TaskIndex>& rec_stack);

    /**
     * DFS helper for reachability check
     */
    bool is_reachable_dfs(std::shared_ptr<Task> current,
                          std::shared_ptr<Task> target,
                          std::unordered_set<TaskIndex>& visited);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H
