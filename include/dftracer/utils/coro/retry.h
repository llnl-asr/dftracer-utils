// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_RETRY_H
#define DFTRACER_UTILS_CORO_RETRY_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/function.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

struct RetryPolicy {
    /// Runs in total, so 1 means no retry.
    std::size_t max_attempts{3};
    std::chrono::milliseconds initial_backoff{10};
    double multiplier{2.0};
    std::chrono::milliseconds max_backoff{1000};
    /// Each wait grows by a random share up to this, in [0, 1].
    double jitter{0.0};
    /// Null retries every error except a cancelled one.
    dstd::Function<bool(std::exception_ptr)> should_retry;
};

namespace detail {
bool is_cancelled_error(const std::exception_ptr& error) noexcept;
std::chrono::milliseconds retry_backoff(const RetryPolicy& policy,
                                        std::size_t attempt);
}  // namespace detail

/**
 * @brief Runs `fn()` until its task succeeds or the policy ends.
 *
 * Takes a function and not a task, because a task runs once. Raises the last
 * error. A cancelled error is never retried. @p fn stays alive in this frame
 * while the task it returns runs.
 */
template <typename Fn,
          typename T = typename std::invoke_result_t<Fn&>::value_type>
CoroTask<T> retry(Fn fn, RetryPolicy policy = {}) {
    for (std::size_t attempt = 1;; ++attempt) {
        std::exception_ptr error;
        try {
            if constexpr (std::is_void_v<T>) {
                co_await std::invoke(fn);
                co_return;
            } else {
                T value = co_await std::invoke(fn);
                co_return value;
            }
        } catch (...) {
            error = std::current_exception();
        }
        const bool may_retry =
            attempt < policy.max_attempts &&
            !detail::is_cancelled_error(error) &&
            (!policy.should_retry || policy.should_retry(error));
        if (!may_retry) common::rethrow_and_clear(error);
        error = nullptr;
        co_await sleep(detail::retry_backoff(policy, attempt));
    }
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_RETRY_H
