#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/task_future.h>

#include <chrono>
#include <thread>

namespace dftracer::utils {

std::any TaskFuture::get() const {
    Executor* current = get_current_executor();

    // If not in an executor context, just wait normally
    if (!current) {
        return inner_future_.get();
    }

    // Work-stealing loop: execute other tasks while waiting
    while (true) {
        // Check if future is ready (non-blocking)
        if (inner_future_.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
            return inner_future_.get();
        }

        // Try to steal and execute a task
        if (!current->try_steal_one_task()) {
            // No tasks available, wait briefly then check again
            if (inner_future_.wait_for(std::chrono::milliseconds(10)) ==
                std::future_status::ready) {
                return inner_future_.get();
            }
        }
        // If we executed a task, immediately check the future again
    }
}

void TaskFuture::wait() const {
    Executor* current = get_current_executor();

    // If not in an executor context, just wait normally
    if (!current) {
        inner_future_.wait();
        return;
    }

    // Work-stealing loop: execute other tasks while waiting
    while (inner_future_.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        // Try to steal and execute a task
        if (!current->try_steal_one_task()) {
            // No tasks available, wait briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

}  // namespace dftracer::utils