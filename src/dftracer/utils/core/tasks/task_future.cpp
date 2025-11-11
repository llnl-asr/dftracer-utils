#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/pipeline/task_item.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_future.h>
#include <dftracer/utils/core/tasks/task_future_impl.h>

namespace dftracer::utils {

void TaskFuture<void>::await_resume() {
    if (!task_) {
        throw std::runtime_error("Invalid TaskFuture");
    }
    task_->get_future().get();
}

void TaskFuture<void>::get() {
    if (!task_) {
        throw std::runtime_error("Invalid TaskFuture");
    }
    task_->get_future().get();
}

}  // namespace dftracer::utils
