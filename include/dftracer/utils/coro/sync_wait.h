// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_SYNC_WAIT_H
#define DFTRACER_UTILS_CORO_SYNC_WAIT_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/run_on.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>

#include <exception>
#include <future>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

template <class T>
struct SyncWaitSlot {
    dstd::Optional<T> value;
};
template <>
struct SyncWaitSlot<void> {};

// The frame owns the promise. The waiter may free its stack as soon as the
// future is ready, while set_value may still be running.
template <class T>
Coro sync_wait_child(CoroTask<T> task, SyncWaitSlot<T>& slot,
                     std::exception_ptr& error, std::promise<void> done) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
        } else {
            T value = co_await std::move(task);
            slot.value.emplace(std::move(value));
        }
    } catch (...) {
        error = std::current_exception();
    }
    done.set_value();
}

}  // namespace detail

/**
 * @brief Runs `task` on `ex` and blocks the calling thread until it ends.
 *
 * The task's error is raised here. A thread of an executor keeps running that
 * executor's work while it waits. On the only thread of an executor that
 * cannot lend it (@ref Executor::drive_until returns false) this raises
 * before the task runs.
 */
template <class T>
T sync_wait(Executor& ex, CoroTask<T> task) {
    Executor* here = Executor::current();
    if (here == &ex) return task.get();
    // A thread of another executor keeps serving it while it waits.
    if (here != nullptr) return run_on(ex, std::move(task)).get();
    task.handle().promise().set_executor(&ex);
    detail::SyncWaitSlot<T> slot;
    std::exception_ptr error;
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    Coro child =
        detail::sync_wait_child(std::move(task), slot, error, std::move(done));
    child.handle().promise().executor = &ex;
    ex.post(child.release());
    finished.wait();
    if (error) common::rethrow_and_clear(error);
    if constexpr (!std::is_void_v<T>) return std::move(*slot.value);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_SYNC_WAIT_H
