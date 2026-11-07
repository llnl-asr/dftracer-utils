// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_TIME_H
#define DFTRACER_UTILS_CORO_TIME_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/when_any.h>

#include <chrono>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/// Waits for @p duration without holding a thread. co_await throws a cancelled
/// error (see coro::is_cancelled) when the executor shuts down first.
template <typename Rep, typename Period>
TimeoutAwaitable<std::chrono::duration<Rep, Period>> sleep(
    std::chrono::duration<Rep, Period> duration) {
    return TimeoutAwaitable<std::chrono::duration<Rep, Period>>(duration,
                                                                nullptr, false);
}

/**
 * @brief Runs @p task and raises a dstd::Condition::Timeout error when
 * @p duration passes first.
 *
 * The task is cancelled at its next wait. It must reach a wait to end, so a
 * task that computes without waiting delays the error until it finishes.
 */
template <typename T, typename Rep, typename Period>
CoroTask<T> with_timeout(CoroTask<T> task,
                         std::chrono::duration<Rep, Period> duration) {
    auto result =
        co_await when_any(std::move(task), timeout(duration, nullptr));
    if constexpr (!std::is_void_v<T>) {
        co_return std::move(result.template get<0>());
    }
}

/// @ref with_timeout with a point in time. A time in the past still starts the
/// task and then cancels it at its first wait.
template <typename T, typename Clock, typename Duration>
CoroTask<T> with_deadline(CoroTask<T> task,
                          std::chrono::time_point<Clock, Duration> deadline) {
    return with_timeout(std::move(task), deadline - Clock::now());
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_TIME_H
