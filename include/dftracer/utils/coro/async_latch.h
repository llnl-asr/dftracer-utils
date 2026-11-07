// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_ASYNC_LATCH_H
#define DFTRACER_UTILS_CORO_ASYNC_LATCH_H

#include <dftracer/utils/coro/async_event.h>

#include <atomic>
#include <cstddef>

namespace dftracer::utils::coro {

/// Single-use counter that coroutines wait on, like std::latch. Thread-safe.
/// A count_down past zero is a caller bug, as it is for std::latch.
class AsyncLatch {
   public:
    explicit AsyncLatch(std::size_t count) noexcept
        : count_(count), reached_zero_(count == 0) {}

    void count_down(std::size_t n = 1) {
        if (count_.fetch_sub(n, std::memory_order_acq_rel) == n) {
            reached_zero_.set();
        }
    }

    bool try_wait() const noexcept { return reached_zero_.is_set(); }

    /// co_await throws a cancelled error (see coro::is_cancelled) when the
    /// executor shuts down while it waits.
    [[nodiscard]] AsyncEvent::WaitAwaitable wait() noexcept {
        return reached_zero_.wait();
    }

   private:
    std::atomic<std::size_t> count_;
    AsyncEvent reached_zero_;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_ASYNC_LATCH_H
