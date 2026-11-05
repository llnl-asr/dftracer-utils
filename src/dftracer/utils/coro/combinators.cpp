// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/retry.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <random>

namespace dftracer::utils::coro::detail {

dstd::Error error_from_exception(const std::exception_ptr& error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const common::DFTUtilsException& e) {
        return e.error();
    } catch (const std::exception& e) {
        return dstd::Error{dstd::Condition::Unknown, e.what()};
    } catch (...) {
        return dstd::Error{dstd::Condition::Unknown, "unknown error"};
    }
}

std::size_t parallel_worker_count(std::size_t requested, std::size_t items,
                                  const Executor* executor) noexcept {
    std::size_t limit = requested;
    if (limit == 0) limit = executor != nullptr ? executor->concurrency() : 1;
    return std::min(std::max<std::size_t>(limit, 1), items);
}

bool is_cancelled_error(const std::exception_ptr& error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const common::DFTUtilsException& e) {
        return is_cancelled(e);
    } catch (...) {
        return false;
    }
}

std::chrono::milliseconds retry_backoff(const RetryPolicy& policy,
                                        std::size_t attempt) {
    thread_local std::minstd_rand t_rng{std::random_device{}()};
    const double base =
        static_cast<double>(policy.initial_backoff.count()) *
        std::pow(policy.multiplier, static_cast<double>(attempt - 1));
    const double capped =
        std::min(base, static_cast<double>(policy.max_backoff.count()));
    std::uniform_real_distribution<double> share(0.0, policy.jitter);
    const double waited =
        policy.jitter > 0.0 ? capped * (1.0 + share(t_rng)) : capped;
    return std::chrono::milliseconds(static_cast<std::int64_t>(waited));
}

}  // namespace dftracer::utils::coro::detail
