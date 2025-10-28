#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>

namespace dftracer::utils {

TaskFuture TaskContext::submit_task(std::shared_ptr<Task> task,
                                    const std::any& input) {
    if (!scheduler_) {
        throw std::runtime_error(
            "TaskContext: No scheduler available for dynamic task submission");
    }

    if (!task) {
        throw std::invalid_argument("TaskContext: Cannot submit null task");
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "Submitting dynamic task '%s' from parent task ID %d",
        task->get_name().c_str(), current_task_id_);

    // Submit task to scheduler
    scheduler_->submit_dynamic_task(task, input);

    // Return task's future wrapped in TaskFuture for automatic work-stealing
    return TaskFuture(task->get_future());
}

}  // namespace dftracer::utils
