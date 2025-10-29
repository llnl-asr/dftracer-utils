#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_CONTEXT_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_CONTEXT_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/executors/executor_task_output.h>
#include <dftracer/utils/core/tasks/task.h>

#include <any>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dftracer::utils {
class Pipeline;

class ExecutorContext {
   private:
    const Pipeline* pipeline_;

   public:
    ExecutorContext(const Pipeline* pipeline) : pipeline_(pipeline) {}
    ~ExecutorContext() = default;

    ExecutorContext(const ExecutorContext&) = delete;
    ExecutorContext& operator=(const ExecutorContext&) = delete;
    ExecutorContext(ExecutorContext&&) = default;
    ExecutorContext& operator=(ExecutorContext&&) = default;

    Task* get_task(TaskIndex index) const;
    const std::vector<TaskIndex>& get_task_dependencies(TaskIndex index) const;
    const std::vector<TaskIndex>& get_task_dependents(TaskIndex index) const;

    TaskIndex add_dynamic_task(std::unique_ptr<Task> task,
                               TaskIndex depends_on = -1);
    void add_dynamic_dependency(TaskIndex from, TaskIndex to);

    Task* get_dynamic_task(TaskIndex index) const;
    const std::vector<TaskIndex>& get_dynamic_dependencies(
        TaskIndex index) const;
    const std::vector<TaskIndex>& get_dynamic_dependents(TaskIndex index) const;

    void set_task_output(TaskIndex index, std::any output);
    std::any get_task_output(TaskIndex index) const;
    void set_task_completed(TaskIndex index, bool completed);
    bool is_task_completed(TaskIndex index) const;

    void increment_user_ref(TaskIndex index);
    void release_user_ref(TaskIndex index);
    std::any consume_task_output(TaskIndex index);

    template <typename O>
    O wait_and_get_result(TaskIndex index) {
        while (!is_task_completed(index)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::any_cast<O>(get_task_output(index));
    }
    void mark_task_completed(TaskIndex index);
    bool wait_for_task_completion(TaskIndex index);

    void set_promise_fulfiller(TaskIndex index,
                               std::function<void(const std::any&)> fulfiller);
    void set_promise_exception_fulfiller(
        TaskIndex index, std::function<void(std::exception_ptr)> fulfiller);
    void fulfill_dynamic_promise(TaskIndex index, const std::any& result) const;
    void fulfill_dynamic_promise_exception(TaskIndex index,
                                           std::exception_ptr exception) const;

    void reset();
    void initialize_task_tracking();

    std::size_t dynamic_task_count() const { return dynamic_tasks_.size(); }

    const Pipeline* get_pipeline() const { return pipeline_; }

    bool validate() const;
    bool is_empty() const;
    bool has_cycles() const;
    bool is_terminal_task(TaskIndex index) const;

   private:
    std::vector<std::unique_ptr<Task>> dynamic_tasks_;
    std::vector<std::vector<TaskIndex>>
        dynamic_dependencies_;  // who depends on this task
    std::vector<std::vector<TaskIndex>>
        dynamic_dependents_;    // who this task depends on

    mutable std::mutex task_outputs_mutex_;
    std::unordered_map<TaskIndex, std::unique_ptr<ExecutorTaskOutput>>
        task_outputs_;
    mutable std::mutex task_completed_mutex_;
    std::unordered_map<TaskIndex, bool> task_completed_;
    mutable std::mutex dependency_count_mutex_;
    std::unordered_map<TaskIndex, int> dependency_count_;
    mutable std::mutex promise_fulfillers_mutex_;
    std::unordered_map<TaskIndex, std::function<void(const std::any&)>>
        promise_fulfillers_;
    std::unordered_map<TaskIndex, std::function<void(std::exception_ptr)>>
        promise_exception_fulfillers_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_CONTEXT_H
