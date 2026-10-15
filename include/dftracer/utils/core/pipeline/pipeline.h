#ifndef DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H
#define DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/pipeline/pipeline_output.h>
#include <dftracer/utils/core/runtime.h>

#include <any>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace dftracer::utils {

class Task;
class Executor;
class Scheduler;

/// DAG container and orchestrator: holds source/destination tasks, validates
/// the DAG (reachability, types, cycles), and delegates execution to the
/// scheduler/executor.
class Pipeline {
   private:
    std::shared_ptr<Task> source_;       ///< Single source (may be NoOpTask)
    std::shared_ptr<Task> destination_;  ///< Can be nullptr

    std::vector<std::shared_ptr<Task>> all_tasks_;

    std::unique_ptr<Runtime> runtime_;
    std::unique_ptr<Scheduler> scheduler_;

    std::string name_;
    bool validated_{false};

    ErrorPolicy error_policy_{ErrorPolicy::FAIL_FAST};
    ErrorHandler error_handler_{nullptr};

   public:
    explicit Pipeline(
        const PipelineConfig& config = PipelineConfig::default_config());

    ~Pipeline();

    /// The Runtime backing this pipeline, for progress and pool introspection.
    const Runtime& runtime() const { return *runtime_; }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    void set_source(std::shared_ptr<Task> source);

    /// Multiple sources auto-create a NoOpTask parent.
    void set_source(std::initializer_list<std::shared_ptr<Task>> sources);

    void set_source(const std::vector<std::shared_ptr<Task>>& sources);

    template <typename... Tasks>
    auto set_source(Tasks&&... sources) -> std::enable_if_t<
        (sizeof...(Tasks) > 1) &&
        (std::is_convertible_v<Tasks, std::shared_ptr<Task>> && ...)> {
        set_source({std::forward<Tasks>(sources)...});
    }

    /// Optional. If unset, all terminal tasks are destinations.
    void set_destination(std::shared_ptr<Task> destination);

    /// Multiple destinations auto-create a NoOpTask child.
    void set_destination(
        std::initializer_list<std::shared_ptr<Task>> destinations);

    void set_destination(
        const std::vector<std::shared_ptr<Task>>& destinations);

    template <typename... Tasks>
    auto set_destination(Tasks&&... destinations) -> std::enable_if_t<
        (sizeof...(Tasks) > 1) &&
        (std::is_convertible_v<Tasks, std::shared_ptr<Task>> && ...)> {
        set_destination({std::forward<Tasks>(destinations)...});
    }

    /// Checks reachability, type compatibility, and cycles.
    bool validate();

    PipelineOutput execute(const std::any& input = std::any{});

    template <typename T, typename = std::enable_if_t<
                              !std::is_same_v<std::decay_t<T>, std::any>>>
    PipelineOutput execute(T&& input) {
        return execute(std::any{std::forward<T>(input)});
    }

    void set_error_policy(ErrorPolicy policy);

    void set_progress_callback(
        std::function<void(std::size_t completed, std::size_t total)> callback);

    const std::string& get_name() const { return name_; }

    std::shared_ptr<Task> get_source() const { return source_; }

    std::shared_ptr<Task> get_destination() const { return destination_; }

   private:
    bool validate_reachability();

    bool has_cycles();

    void collect_all_tasks();

    void collect_tasks_dfs(std::shared_ptr<Task> task,
                           std::unordered_set<TaskIndex>& visited);

    bool has_cycles_dfs(std::shared_ptr<Task> task,
                        std::unordered_set<TaskIndex>& visited,
                        std::unordered_set<TaskIndex>& rec_stack);

    bool is_reachable_dfs(std::shared_ptr<Task> current,
                          std::shared_ptr<Task> target,
                          std::unordered_set<TaskIndex>& visited);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H
