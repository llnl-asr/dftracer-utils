#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/sequential_scheduler.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/thread_scheduler.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/function_task.h>
#include <dftracer/utils/core/tasks/task_context.h>

namespace dftracer::utils {

void TaskContext::add_dependency(TaskIndex from, TaskIndex to) {
    if (!execution_context_) {
        throw std::runtime_error("TaskContext: No execution context available");
    }

    // ExecutorContext handles both static and dynamic dependencies
    execution_context_->add_dynamic_dependency(from, to);
}

void TaskContext::schedule(TaskIndex task_id, const std::any& input) {
    if (!scheduler_ || !execution_context_) {
        return;
    }

    Task* task_ptr = execution_context_->get_dynamic_task(task_id);
    if (!task_ptr) {
        return;
    }

    if (auto* thread_scheduler = dynamic_cast<ThreadScheduler*>(scheduler_)) {
        thread_scheduler->submit_dynamic_task(task_id, task_ptr, input);
    } else if (auto* seq_scheduler =
                   dynamic_cast<SequentialScheduler*>(scheduler_)) {
        seq_scheduler->submit_dynamic_task(task_id, task_ptr, input);
    }
}

}  // namespace dftracer::utils
