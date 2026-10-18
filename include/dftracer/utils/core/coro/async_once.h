#ifndef DFTRACER_UTILS_CORE_CORO_ASYNC_ONCE_H
#define DFTRACER_UTILS_CORE_CORO_ASYNC_ONCE_H

#include <dftracer/utils/core/coro/completion_latch.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/executor/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <utility>

namespace dftracer::utils {
class Executor;
namespace coro {

/**
 * Single-flight async value with a broadcast wake, lock-free.
 *
 * The first caller of get() becomes the leader and runs the producer; every
 * other concurrent caller suspends onto an intrusive stack and is resumed
 * together the instant the leader completes (all at once, not handed off
 * one-by-one). Once ready, later callers return the value without suspending.
 * T must be cheap to copy (e.g. a shared_ptr); it is set exactly once.
 *
 * No std::mutex: a single atomic encodes not-started / in-progress (with a
 * Treiber stack of waiters) / ready, so a waiting worker thread suspends its
 * coroutine rather than blocking. Same discipline as AsyncMutex.
 */
template <typename T>
class AsyncOnce {
   public:
    AsyncOnce() = default;
    AsyncOnce(const AsyncOnce&) = delete;
    AsyncOnce& operator=(const AsyncOnce&) = delete;

    template <typename Producer>
    coro::CoroTask<T> get(Producer produce) {
        std::uintptr_t s = state_.load(std::memory_order_acquire);
        if (s == READY) co_return value_;

        if (s == NOT_STARTED && state_.compare_exchange_strong(
                                    s, IN_PROGRESS, std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
            // Leader: produce, publish, then wake the whole waiter stack.
            value_ = co_await produce();
            std::uintptr_t old =
                state_.exchange(READY, std::memory_order_acq_rel);
            for (Waiter* w = to_stack(old); w != nullptr;) {
                Waiter* next =
                    w->next;  // w may resume + destroy before we read
                resume_continuation(w->executor, w->handle);
                w = next;
            }
            co_return value_;
        }

        // Not the leader (or the CAS lost): wait for the leader to publish.
        co_await Awaiter{this};
        co_return value_;
    }

   private:
    /// NOT_STARTED / IN_PROGRESS / READY are distinct from any Waiter* (which
    /// is pointer-aligned, so never 0/1/2).
    static constexpr std::uintptr_t NOT_STARTED = 0;
    static constexpr std::uintptr_t IN_PROGRESS =
        1;  ///< in progress, no waiters
    static constexpr std::uintptr_t READY = 2;

    struct Waiter {
        std::coroutine_handle<> handle;
        Executor* executor;
        Waiter* next;
    };

    static Waiter* to_stack(std::uintptr_t s) {
        return (s == IN_PROGRESS || s == READY || s == NOT_STARTED)
                   ? nullptr
                   : reinterpret_cast<Waiter*>(s);
    }

    struct Awaiter {
        AsyncOnce* self;
        Waiter waiter{};

        bool await_ready() const noexcept {
            return self->state_.load(std::memory_order_acquire) == READY;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            waiter.handle = h;
            waiter.executor = resume_executor_for(nullptr);
            std::uintptr_t old = self->state_.load(std::memory_order_acquire);
            while (true) {
                if (old == READY) return false;  // published; resume now
                waiter.next = to_stack(old);
                if (self->state_.compare_exchange_weak(
                        old, reinterpret_cast<std::uintptr_t>(&waiter),
                        std::memory_order_release, std::memory_order_acquire)) {
                    return true;  // pushed; stay suspended
                }
            }
        }

        void await_resume() const noexcept {}
    };

    std::atomic<std::uintptr_t> state_{NOT_STARTED};
    T value_{};
};

}  // namespace coro
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_CORO_ASYNC_ONCE_H
