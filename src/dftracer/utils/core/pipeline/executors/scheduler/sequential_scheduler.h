#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_SEQUENTIAL_SCHEDULER_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_SEQUENTIAL_SCHEDULER_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>

#include <any>
#include <functional>
#include <queue>
#include <unordered_map>

namespace dftracer::utils {
class Pipeline;
class ExecutorContext;

class SequentialScheduler : public Scheduler {
   private:
    struct TaskItem {
        TaskIndex task_id;
        Task* task_ptr;
        std::any input;

        TaskItem(TaskIndex id, Task* ptr, std::any inp)
            : task_id(id), task_ptr(ptr), input(std::move(inp)) {}
    };

    std::queue<TaskItem> task_queue_;
    std::unordered_map<TaskIndex, bool> task_completed_;
    std::unordered_map<TaskIndex, std::size_t> dependency_count_;
    ExecutorContext* current_execution_context_;
    const Pipeline* current_pipeline_;

   public:
    SequentialScheduler();
    ~SequentialScheduler() = default;

    PipelineOutput execute(const Pipeline& pipeline,
                           const std::any& input) override;
    void submit_dynamic_task(TaskIndex task_id, Task* task_ptr,
                             const std::any& input);

   private:
    void execute_task_with_dependencies(ExecutorContext& execution_context,
                                        TaskIndex task_id,
                                        const std::any& input);
    void process_all_tasks();
    void process_remaining_dynamic_tasks();
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_SEQUENTIAL_SCHEDULER_H
