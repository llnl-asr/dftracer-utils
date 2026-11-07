// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_ASYNC_EVENT_H
#define DFTRACER_UTILS_CORO_ASYNC_EVENT_H

#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <mutex>

namespace dftracer::utils::coro {

class Executor;

/**
 * @brief A flag that coroutines wait on. @ref set wakes every waiter and later
 * waits return at once, until @ref reset.
 *
 * Thread-safe. The event must outlive its waiters.
 */
class AsyncEvent {
   public:
    /// co_await throws a cancelled error (see coro::is_cancelled) when the
    /// executor shuts down while it waits.
    class WaitAwaitable {
       public:
        explicit WaitAwaitable(AsyncEvent& event) noexcept : event_(event) {}
        ~WaitAwaitable();

        WaitAwaitable(const WaitAwaitable&) = delete;
        WaitAwaitable& operator=(const WaitAwaitable&) = delete;

        // Always takes the lock, so a waiter that goes on and frees the event
        // cannot do it while @ref set still holds the lock.
        bool await_ready() const noexcept { return false; }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) {
            detail::set_awaiting_async(h, true);
            if (suspend(h, detail::executor_for(h))) return true;
            detail::set_awaiting_async(h, false);
            return false;
        }

        void await_resume();

       private:
        friend class AsyncEvent;

        bool suspend(std::coroutine_handle<> h, Executor* executor);
        static void cancel_thunk(void* self) noexcept;

        AsyncEvent& event_;
        std::coroutine_handle<> handle_;
        Executor* executor_{nullptr};
        WaitAwaitable* next_{nullptr};
        detail::ParkNode* park_{nullptr};
        bool queued_{false};
        bool cancelled_{false};
    };

    AsyncEvent() = default;
    explicit AsyncEvent(bool initially_set) noexcept : set_(initially_set) {}

    AsyncEvent(const AsyncEvent&) = delete;
    AsyncEvent& operator=(const AsyncEvent&) = delete;

    [[nodiscard]] WaitAwaitable wait() noexcept { return WaitAwaitable(*this); }

    void set();

    void reset() noexcept;

    bool is_set() const noexcept {
        return set_.load(std::memory_order_acquire);
    }

   private:
    bool remove_locked(WaitAwaitable* waiter) noexcept;

    std::atomic<bool> set_{false};
    std::mutex mutex_;
    WaitAwaitable* head_{nullptr};
    WaitAwaitable* tail_{nullptr};
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_ASYNC_EVENT_H
