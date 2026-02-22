#ifndef DFTRACER_UTILS_CORE_CORO_JOIN_HANDLE_H
#define DFTRACER_UTILS_CORE_CORO_JOIN_HANDLE_H

#include <dftracer/utils/core/coro/coro.h>

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace dftracer::utils::coro {

/// Stack-allocated join barrier for Coro instances.
///
/// Uses the cppcoro when_all_counter pattern.  pending_ is initialised
/// to 1 (the joiner's slot) and incremented by 1 for every tracked Coro.
/// Each Coro's FinalAwaiter and the joiner's await_suspend each call
/// fetch_sub(1).  Whichever decrement brings the counter to zero
/// resumes the joiner -- FinalAwaiter via symmetric transfer,
/// await_suspend by returning the awaiting handle directly.
///
/// Initialising the joiner's slot in the constructor (rather than in
/// join()) ensures that a Coro completing before join() is called can
/// never observe prev==1 and attempt to exchange on continuation_
/// before the joiner has published it.
///
/// Usage:
///   JoinHandle jh;
///   jh.track(coro1);
///   jh.track(coro2);
///   // ... enqueue coroutines to executor ...
///   co_await jh.join();  // suspends until all complete
class JoinHandle {
   public:
    JoinHandle() = default;

    // Non-copyable, non-movable (stack-bound lifetime)
    JoinHandle(const JoinHandle&) = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;
    JoinHandle(JoinHandle&&) = delete;
    JoinHandle& operator=(JoinHandle&&) = delete;

    /// Register a Coro with this join group.
    /// Must be called BEFORE the coroutine is enqueued for execution.
    void track(Coro& c) {
        pending_.fetch_add(1, std::memory_order_relaxed);
        auto& p = c.handle().promise();
        p.join_counter = &pending_;
        p.join_continuation = &continuation_;
    }

    struct JoinAwaitable {
        JoinHandle* handle;

        bool await_ready() const noexcept {
            // Always suspend.  The counter includes the joiner's slot,
            // so we must go through await_suspend to decrement it.
            return false;
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> awaiting) noexcept {
            // Publish continuation, then decrement the joiner's slot.
            // After fetch_sub, do NOT touch `handle` again.
            handle->continuation_.store(awaiting.address(),
                                        std::memory_order_release);
            if (handle->pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                return awaiting;
            }
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    JoinAwaitable join() {
        // The joiner's slot was already added in the constructor.
        return JoinAwaitable{this};
    }

    std::size_t pending() const {
        return pending_.load(std::memory_order_acquire);
    }

   private:
    std::atomic<std::size_t> pending_{1};  // 1 = joiner's slot
    std::atomic<void*> continuation_{nullptr};
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_JOIN_HANDLE_H
