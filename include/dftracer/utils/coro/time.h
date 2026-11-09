// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_TIME_H
#define DFTRACER_UTILS_CORO_TIME_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/when_any.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>

#include <chrono>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/// Waits for @p duration without holding a thread. The task stops when the
/// executor shuts down first.
template <typename Rep, typename Period>
TimeoutAwaitable<std::chrono::duration<Rep, Period>> sleep(
    std::chrono::duration<Rep, Period> duration) {
    return TimeoutAwaitable<std::chrono::duration<Rep, Period>>(duration,
                                                                nullptr);
}

/**
 * @brief Runs @p task and gives a dstd::Condition::Timeout error when
 * @p duration passes first.
 *
 * The task is cancelled at its next wait. It must reach a wait to end, so a
 * task that computes without waiting delays the error until it finishes.
 */
template <typename T, typename Rep, typename Period>
CoroTask<dstd::Result<T>> with_timeout(
    CoroTask<T> task, std::chrono::duration<Rep, Period> duration) {
    auto result =
        co_await when_any(std::move(task), timeout(duration, nullptr));
    if (result.index != 0) {
        co_return dstd::Unexpected(
            dstd::Error{dstd::Condition::Timeout, "Operation timed out"});
    }
    if constexpr (std::is_void_v<T>) {
        co_return dstd::Result<void>{};
    } else {
        co_return dstd::Result<T>(std::move(result.template get<0>()));
    }
}

/// @ref with_timeout with a point in time. A time in the past still starts the
/// task and then cancels it at its first wait.
template <typename T, typename Clock, typename Duration>
CoroTask<dstd::Result<T>> with_deadline(
    CoroTask<T> task, std::chrono::time_point<Clock, Duration> deadline) {
    return with_timeout(std::move(task), deadline - Clock::now());
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_TIME_H
