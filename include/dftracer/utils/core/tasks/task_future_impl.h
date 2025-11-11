#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_IMPL_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_IMPL_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_future.h>

namespace dftracer::utils {

template <typename T>
template <typename Promise>
std::coroutine_handle<> TaskFuture<T>::await_suspend(
    std::coroutine_handle<Promise> awaiting) {
    if (!task_ || !scheduler_) {
        return awaiting;
    }

    if (completed_->load(std::memory_order_acquire)) {
        return awaiting;
    }

    if (task_->is_completed()) {
        completed_->store(true, std::memory_order_release);
        return awaiting;
    }

    if constexpr (std::is_base_of_v<coro::PromiseBase, Promise>) {
        auto* root = awaiting.promise().get_root_promise();
        root->set_awaited_task_id(task_id_);
        root->awaiting_async_ = true;
    }

    auto* executor = scheduler_->get_executor();
    scheduler_->register_task_completion_callback(
        task_id_, [awaiting, executor]() {
            if (executor) {
                executor->schedule_coroutine_resumption(awaiting);
            } else {
                awaiting.resume();
            }
        });

    return std::noop_coroutine();
}

template <typename T>
T TaskFuture<T>::await_resume() {
    if (!task_) {
        throw std::runtime_error("Invalid TaskFuture");
    }

    std::any result_any = task_->get_future().get();

    if constexpr (!std::is_void_v<T>) {
        if constexpr (std::is_same_v<T, std::any>) {
            return result_any;
        } else {
            return std::any_cast<T>(result_any);
        }
    }
}

template <typename Promise>
std::coroutine_handle<> TaskFuture<void>::await_suspend(
    std::coroutine_handle<Promise> awaiting) {
    if (!task_ || !scheduler_) {
        return awaiting;
    }

    if (completed_->load(std::memory_order_acquire)) {
        return awaiting;
    }

    if (task_->is_completed()) {
        completed_->store(true, std::memory_order_release);
        return awaiting;
    }

    if constexpr (std::is_base_of_v<coro::PromiseBase, Promise>) {
        auto* root = awaiting.promise().get_root_promise();
        root->set_awaited_task_id(task_id_);
        root->awaiting_async_ = true;
    }

    auto* executor = scheduler_->get_executor();
    scheduler_->register_task_completion_callback(
        task_id_, [awaiting, executor]() {
            if (executor) {
                executor->schedule_coroutine_resumption(awaiting);
            } else {
                awaiting.resume();
            }
        });

    return std::noop_coroutine();
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_IMPL_H
