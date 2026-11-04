// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_PIPELINE_PIPELINE_H
#define DFTRACER_UTILS_RUNTIME_PIPELINE_PIPELINE_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/pipeline/pipeline_config.h>
#include <dftracer/utils/runtime/pipeline/pipeline_output.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/error.h>

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
class Scheduler;
class Watchdog;

/// DAG container and orchestrator: holds source/destination tasks, validates
/// the DAG (reachability, types, cycles), and delegates execution to the
/// scheduler/executor.
class Pipeline {
   private:
    dstd::SharedPtr<Task> source_;       ///< Single source (may be NoOpTask)
    dstd::SharedPtr<Task> destination_;  ///< Can be nullptr

    std::vector<dstd::SharedPtr<Task>> all_tasks_;

    dstd::SharedPtr<Runtime> runtime_;
    std::unique_ptr<Watchdog> watchdog_;
    std::unique_ptr<Scheduler> scheduler_;

    std::string name_;
    bool validated_{false};

    ErrorPolicy error_policy_{ErrorPolicy::FailFast};
    ErrorHandler error_handler_{nullptr};

   public:
    explicit Pipeline(
        const PipelineConfig& config = PipelineConfig::default_config());

    ~Pipeline();

    /// The process runtime this pipeline runs on, for progress and pool
    /// introspection.
    const Runtime& runtime() const { return *runtime_; }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    void set_source(dstd::SharedPtr<Task> source);

    /// Multiple sources auto-create a @ref NoOpTask parent.
    void set_source(std::initializer_list<dstd::SharedPtr<Task>> sources);

    void set_source(const std::vector<dstd::SharedPtr<Task>>& sources);

    template <typename... Tasks>
    auto set_source(Tasks&&... sources) -> std::enable_if_t<
        (sizeof...(Tasks) > 1) &&
        (std::is_convertible_v<Tasks, dstd::SharedPtr<Task>> && ...)> {
        set_source({std::forward<Tasks>(sources)...});
    }

    /// Optional. If unset, all terminal tasks are destinations.
    void set_destination(dstd::SharedPtr<Task> destination);

    /// Multiple destinations auto-create a @ref NoOpTask child.
    void set_destination(
        std::initializer_list<dstd::SharedPtr<Task>> destinations);

    void set_destination(
        const std::vector<dstd::SharedPtr<Task>>& destinations);

    template <typename... Tasks>
    auto set_destination(Tasks&&... destinations) -> std::enable_if_t<
        (sizeof...(Tasks) > 1) &&
        (std::is_convertible_v<Tasks, dstd::SharedPtr<Task>> && ...)> {
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

    const std::string& name() const { return name_; }

    dstd::SharedPtr<Task> source() const { return source_; }

    dstd::SharedPtr<Task> destination() const { return destination_; }

   private:
    bool validate_reachability();

    bool has_cycles();

    void collect_all_tasks();

    void collect_tasks_dfs(dstd::SharedPtr<Task> task,
                           std::unordered_set<common::TaskIndex>& visited);

    bool has_cycles_dfs(dstd::SharedPtr<Task> task,
                        std::unordered_set<common::TaskIndex>& visited,
                        std::unordered_set<common::TaskIndex>& rec_stack);

    bool is_reachable_dfs(dstd::SharedPtr<Task> current,
                          dstd::SharedPtr<Task> target,
                          std::unordered_set<common::TaskIndex>& visited);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_PIPELINE_PIPELINE_H
