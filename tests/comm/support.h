// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_COMM_SUPPORT_H
#define DFTRACER_UTILS_TESTS_COMM_SUPPORT_H

// Helpers shared by the comm tests: a rank runner, an executor that only
// queues, and the clock the deadlines use.

#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <task_result.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace dftu_comm_test {

using dftracer::utils::coro::CoroTask;
using dftracer::utils::coro::Scope;
using dftracer::utils::dstd::Error;
using dftracer::utils::dstd::Optional;
using dftracer::utils::dstd::Result;
using dftracer::utils::dstd::StringView;
using dftracer::utils::dstd::Vector;

/// A deadline @p ns nanoseconds from now, in the unit of the deadlines.
inline std::uint64_t deadline_after(std::uint64_t ns) noexcept {
    return dftracer::utils::dstd::steady_now_ns() + ns;
}

/// True when @p error names @p text.
inline bool has_text(const Error& error, StringView text) {
    return error.message().find(text) != StringView::npos;
}

/// How many times @p error names @p text.
inline std::size_t count_text(const Error& error, StringView text) {
    const StringView message = error.message();
    const StringView needle = text;
    std::size_t count = 0;
    std::size_t at = message.find(needle);
    while (at != StringView::npos) {
        ++count;
        at = message.find(needle, at + needle.size());
    }
    return count;
}

/// An executor that only queues, so a test thread decides when a woken
/// coroutine runs and on which thread. It registers parked waiters, as a
/// shutting-down executor reaches them.
struct QueueExecutor : dftracer::utils::coro::Executor {
    void post(std::coroutine_handle<> handle) noexcept override {
        if (!handle) return;
        std::lock_guard<std::mutex> lock(mutex);
        ready.push_back(handle);
        posted.fetch_add(1, std::memory_order_relaxed);
    }

    /// Runs the queued handles, then the handles they post.
    void run() {
        for (;;) {
            std::coroutine_handle<> handle;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (ready.empty()) break;
                handle = ready.front();
                ready.erase(ready.begin());
            }
            if (!handle.done()) {
                dftracer::utils::coro::resume_on(*this, handle);
            }
        }
        dftracer::utils::coro::drain_thread_local_destroys();
    }

    std::size_t queued() {
        std::lock_guard<std::mutex> lock(mutex);
        return ready.size();
    }

    dftracer::utils::coro::detail::ParkNode* park(
        void* waiter,
        dftracer::utils::coro::detail::ParkCancelFn cancel) noexcept override {
        return registry.park(waiter, cancel);
    }

    void unpark(
        dftracer::utils::coro::detail::ParkNode* node) noexcept override {
        registry.unpark(node);
    }

    void wake_cancelled() noexcept override {
        registry.cancel_requested_waiters();
    }

    /// Wakes every parked waiter, as a shutdown does.
    void cancel_all() noexcept { registry.cancel_all(); }

    /// Waiters parked here now.
    std::size_t parked() const noexcept { return registry.size(); }

    std::mutex mutex;
    Vector<std::coroutine_handle<>> ready;
    std::atomic<int> posted{0};
    dftracer::utils::coro::detail::ParkRegistry registry;
};

/// Runs `body(rank)` for every rank at once on @p rt, blocks until they end,
/// and gives the error each rank ended with, if any.
template <class Body>
Vector<Optional<Error>> run_ranks(dftracer::utils::Runtime& rt,
                                  std::uint32_t ranks, Body body) {
    Vector<Optional<Error>> errors(ranks);
    auto driver = dftracer::utils::coro::with_scope(
        [ranks, &body, &errors](Scope& scope) -> CoroTask<void> {
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                scope.spawn([&body, &errors, rank]() -> CoroTask<void> {
                    auto outcome = co_await body(rank);
                    if (!outcome) errors[rank] = std::move(outcome).error();
                });
            }
            co_return;
        });
    auto result = rt.block_on(std::move(driver));
    dftu_utils_test::ok(std::move(result));
    return errors;
}

/// Yields this coroutine until @p flag holds.
inline CoroTask<void> wait_flag(std::atomic<bool>& flag) {
    bool waiting = true;
    while (waiting) {
        co_await dftracer::utils::coro::yield();
        waiting = !flag.load(std::memory_order_acquire);
    }
}

/// Yields this coroutine until @p counter reaches @p wanted.
inline CoroTask<void> wait_count(std::atomic<int>& counter, int wanted) {
    bool waiting = true;
    while (waiting) {
        co_await dftracer::utils::coro::yield();
        waiting = counter.load(std::memory_order_acquire) < wanted;
    }
}

/// Spins on @p holds until it is true, and gives false when @p limit passes:
/// the limit is only a net against a hang, never the thing a test waits for.
template <class Pred>
bool spin_until(Pred holds,
                std::chrono::seconds limit = std::chrono::seconds(20)) {
    const auto stop = dftracer::utils::dstd::steady_now() + limit;
    while (!holds()) {
        if (dftracer::utils::dstd::steady_now() >= stop) return false;
        std::this_thread::yield();
    }
    return true;
}

}  // namespace dftu_comm_test

#endif  // DFTRACER_UTILS_TESTS_COMM_SUPPORT_H
