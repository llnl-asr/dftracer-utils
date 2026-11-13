// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_SYNC_WAIT_H
#define DFTRACER_UTILS_CORO_SYNC_WAIT_H

#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/run_on.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/optional.h>

#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

template <class T>
struct SyncWaitSlot {
    dstd::Optional<T> value;
    dstd::Optional<dstd::Error> error;
    bool stopped{false};
};
template <>
struct SyncWaitSlot<void> {
    dstd::Optional<dstd::Error> error;
    bool stopped{false};
};

// The frame owns the promise. The waiter may free its stack as soon as the
// future is ready, while set_value may still be running.
template <class T>
detail::Coro sync_wait_child(CoroTask<T> task, SyncWaitSlot<T>& slot,
                             dstd::Promise<void> done) {
    auto settled = co_await settle(std::move(task));
    slot.error = std::move(settled.error);
    slot.stopped = settled.stopped;
    if constexpr (!std::is_void_v<T>) slot.value = std::move(settled.value);
    done.set_value();
}

}  // namespace detail

/**
 * @brief Runs `task` on `ex` and blocks the calling thread until it ends.
 *
 * Gives the value, or an error with dstd::Condition::Cancelled when the task
 * stopped, or the error the task ended with (see @ref TaskResult).
 * A thread of an executor keeps running that executor's work while it waits. On
 * the only thread of an executor that cannot lend it (@ref
 * Executor::drive_until returns false) this ends the process before the task
 * runs.
 */
template <class T>
TaskResult<T> sync_wait(Executor& ex, CoroTask<T> task) {
    Executor* here = Executor::current();
    if (here == &ex) return task.get();
    // A thread of another executor keeps serving it while it waits.
    if (here != nullptr) return run_on(ex, std::move(task)).get();
    task.handle().promise().set_executor(&ex);
    detail::SyncWaitSlot<T> slot;
    dstd::Promise<void> done;
    dstd::Future<void> finished = done.get_future();
    detail::Coro child =
        detail::sync_wait_child(std::move(task), slot, std::move(done));
    child.handle().promise().executor = &ex;
    ex.post(child.release());
    finished.wait();
    if (slot.error)
        return dstd::Unexpected<dstd::Error>(std::move(*slot.error));
    if (slot.stopped) return detail::stopped_result<T>();
    if constexpr (std::is_void_v<T>) {
        return {};
    } else {
        return TaskResult<T>(std::move(*slot.value));
    }
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_SYNC_WAIT_H
