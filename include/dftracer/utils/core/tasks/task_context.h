#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/tasks/task_future.h>

#include <any>
#include <future>
#include <memory>
#include <type_traits>

namespace dftracer::utils {

class Task;
class Scheduler;

/**
 * TaskContext - Context provided to tasks during execution
 *
 * Features:
 * - Submit dynamic tasks for intra-task parallelism
 * - Access to current task ID
 * - Access to scheduler for dynamic task submission
 */
class TaskContext {
   private:
    Scheduler* scheduler_;
    TaskIndex current_task_id_;

   public:
    /**
     * Constructor
     * @param scheduler Pointer to scheduler
     * @param current_task_id ID of the currently executing task
     */
    TaskContext(Scheduler* scheduler, TaskIndex current_task_id)
        : scheduler_(scheduler), current_task_id_(current_task_id) {}

    /**
     * Submit a dynamic task for execution
     *
     * This allows intra-task parallelism where a task can spawn child tasks
     * dynamically during execution.
     *
     * @param task The task to submit
     * @param input Input for the task
     * @return TaskFuture for the task's result (with automatic work-stealing)
     *
     * Example:
     *   auto child_task = make_task([](int x) { return x * 2; });
     *   auto future = ctx.submit_task(child_task, std::any(42));
     *   int result = std::any_cast<int>(future.get()); // 84
     */
    TaskFuture submit_task(std::shared_ptr<Task> task,
                           const std::any& input = {});

    template <typename T, typename = std::enable_if_t<
                              !std::is_same_v<std::decay_t<T>, std::any>>>
    TaskFuture submit_task(std::shared_ptr<Task> task, T&& input) {
        return submit_task(task, std::any(std::forward<T>(input)));
    }

    /**
     * Get current task ID
     */
    TaskIndex current() const { return current_task_id_; }

    /**
     * Get scheduler reference (for advanced use cases)
     */
    Scheduler* get_scheduler() const { return scheduler_; }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H
