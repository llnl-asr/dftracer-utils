#ifndef DFTRACER_UTILS_RUNTIME_CORO_ASYNC_ONCE_H
#define DFTRACER_UTILS_RUNTIME_CORO_ASYNC_ONCE_H

#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/parked.h>
#include <dftracer/utils/runtime/executor/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::coro {

/**
 * Single-flight async value with a broadcast wake.
 *
 * The first caller of get() becomes the leader and runs the producer; every
 * other concurrent caller suspends and is resumed together the instant the
 * leader completes (all at once, not handed off one-by-one). Once ready,
 * later callers return the value without suspending or taking a lock. T must
 * be cheap to copy (e.g. a shared_ptr); it is set exactly once.
 *
 * A waiter that is still suspended when its executor shuts down is resumed
 * with a cancelled error (see coro::is_cancelled), so get() throws.
 */
template <typename T>
class AsyncOnce {
   public:
    AsyncOnce() = default;
    AsyncOnce(const AsyncOnce&) = delete;
    AsyncOnce& operator=(const AsyncOnce&) = delete;

    template <typename Producer>
    coro::CoroTask<T> get(Producer produce) {
        std::uint8_t s = state_.load(std::memory_order_acquire);
        if (s == READY) co_return value_;

        if (s == NOT_STARTED && state_.compare_exchange_strong(
                                    s, IN_PROGRESS, std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
            value_ = co_await produce();
            Awaiter* w;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.store(READY, std::memory_order_release);
                w = waiters_;
                waiters_ = nullptr;
            }
            while (w != nullptr) {
                Awaiter* next = w->next_;  // w may resume + destroy before we
                                           // read it
                resume_continuation(w->executor_, w->handle_);
                w = next;
            }
            co_return value_;
        }

        // A named awaiter: GCC 12.1 destroys a co_await temporary twice, and
        // this destructor unparks the executor.
        Awaiter waiter{this};
        co_await waiter;
        co_return value_;
    }

   private:
    static constexpr std::uint8_t NOT_STARTED = 0;
    static constexpr std::uint8_t IN_PROGRESS = 1;
    static constexpr std::uint8_t READY = 2;

    class Awaiter {
       public:
        explicit Awaiter(AsyncOnce* self) noexcept : self_(self) {}

        Awaiter(const Awaiter& other) noexcept : self_(other.self_) {}

        ~Awaiter() {
            if (park_) executor_->unpark(park_);
        }

        bool await_ready() const noexcept {
            return self_->state_.load(std::memory_order_acquire) == READY;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            handle_ = h;
            executor_ = resume_executor_for(nullptr);
            std::lock_guard<std::mutex> lock(self_->mutex_);
            if (self_->state_.load(std::memory_order_relaxed) == READY)
                return false;
            next_ = self_->waiters_;
            self_->waiters_ = this;
            if (executor_) {
                park_ = executor_->park(this, &Awaiter::cancel_thunk);
            }
            return true;
        }

        void await_resume() {
            if (park_) {
                executor_->unpark(park_);
                park_ = nullptr;
            }
            if (cancelled_) throw_cancelled("once wait cancelled");
        }

        static void cancel_thunk(void* self) noexcept {
            static_cast<Awaiter*>(self)->cancel_parked();
        }

        void cancel_parked() noexcept {
            {
                std::unique_lock<std::mutex> lock(self_->mutex_,
                                                  std::try_to_lock);
                if (!lock) return;
                Awaiter** link = &self_->waiters_;
                while (*link != nullptr && *link != this)
                    link = &(*link)->next_;
                if (*link == nullptr) return;
                *link = next_;
                next_ = nullptr;
                cancelled_ = true;
            }
            resume_continuation(executor_, handle_);
        }

       private:
        friend class AsyncOnce;

        AsyncOnce* self_;
        std::coroutine_handle<> handle_{};
        Executor* executor_ = nullptr;
        Awaiter* next_ = nullptr;
        ParkNode* park_ = nullptr;
        bool cancelled_ = false;
    };

    std::atomic<std::uint8_t> state_{NOT_STARTED};
    std::mutex mutex_;
    Awaiter* waiters_ = nullptr;
    T value_{};
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_ASYNC_ONCE_H
