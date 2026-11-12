// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_TASK_HANDLE_H
#define DFTRACER_UTILS_RUNTIME_TASK_HANDLE_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <chrono>
#include <utility>

namespace dftracer::utils {

namespace detail {
/// Fulfills `promise` with `result` and marks the stored copy read, so a
/// handle nobody reads does not abort on its unread error. `future` is the
/// shared future of `promise`.
inline void fulfill(dstd::Promise<dstd::Result<void>>& promise,
                    const dstd::SharedFuture<dstd::Result<void>>& future,
                    dstd::Result<void> result) {
    promise.set_value(std::move(result));
    future.get().discard();
}
}  // namespace detail

/// Handle to a submitted void task. Non-blocking by default.
/// Call .wait() or .get() to block, or .done() to check. They give the error
/// of a task that was stopped (dstd::Condition::Cancelled) or that a shut down
/// runtime refused, and rethrow an exception the task raised.
struct TaskHandle {
    dstd::SharedFuture<dstd::Result<void>> future;
    common::TaskIndex id{-1};
    dstd::String name;

    dstd::Result<void> get() { return future.get(); }
    dstd::Result<void> wait() { return future.get(); }
    bool done() const {
        return future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
    }
};

/// Typed handle that can return a value via .get(), as a
/// @ref coro::TaskResult.
/// .wait() does the same and discards the value.
///
/// The result is held via shared_ptr so get() can move it out even for a
/// move-only T (a shared_future's get() only yields a const ref). get() is
/// therefore single-use for a move-only T: a second get() sees a moved-from
/// value.
template <typename T>
struct TypedTaskHandle {
    dstd::SharedFuture<dstd::SharedPtr<coro::TaskResult<T>>> future;
    common::TaskIndex id{-1};
    dstd::String name;

    coro::TaskResult<T> get() { return std::move(*future.get()); }
    dstd::Result<void> wait() {
        const auto& result = *future.get();
        if (!result) return dstd::Unexpected<dstd::Error>(result.error());
        return {};
    }
    bool done() const {
        return future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_TASK_HANDLE_H
