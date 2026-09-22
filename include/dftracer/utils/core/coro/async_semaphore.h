#ifndef DFTRACER_UTILS_CORE_CORO_ASYNC_SEMAPHORE_H
#define DFTRACER_UTILS_CORE_CORO_ASYNC_SEMAPHORE_H

#include <dftracer/utils/core/coro/resumption_helper.h>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace dftracer::utils::coro {

/**
 * @brief Async counting semaphore with variable-sized permits.
 *
 * A coroutine acquires `n` permits and suspends (without blocking its thread)
 * until that many are available; releasing wakes waiters in FIFO order. An
 * acquire larger than the capacity is clamped, so an over-budget unit still
 * runs once the semaphore is otherwise idle (no deadlock). The mutex is held
 * only across the counter/queue update, never across a suspension, and woken
 * waiters are re-enqueued on the executor rather than resumed inline, so
 * release() does not recurse into resumed coroutines.
 */
class CoroSemaphore {
   public:
    explicit CoroSemaphore(std::uint64_t capacity) noexcept
        : capacity_(capacity), available_(capacity) {}

    CoroSemaphore(const CoroSemaphore&) = delete;
    CoroSemaphore& operator=(const CoroSemaphore&) = delete;

    class AcquireOperation {
       public:
        AcquireOperation(CoroSemaphore& sem, std::uint64_t n) noexcept
            : sem_(sem), need_(n > sem.capacity_ ? sem.capacity_ : n) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            // Captured on the acquirer's worker (acquire is only co_awaited
            // from a coroutine already running on an executor), so release()
            // can resume through a known-valid executor instead of the
            // releaser's current thread, which may be off-executor (an
            // io-completion thread) where yield_to_executor would silently drop
            // the resume.
            dftracer::utils::Executor* exec = resume_executor_for(nullptr);
            std::lock_guard<std::mutex> lock(sem_.mtx_);
            if (sem_.waiters_.empty() && sem_.available_ >= need_) {
                sem_.available_ -= need_;
                return false;
            }
            sem_.waiters_.push_back(Waiter{need_, h, exec});
            return true;
        }

        void await_resume() const noexcept {}

       private:
        CoroSemaphore& sem_;
        std::uint64_t need_;
    };

    /// Acquire `n` permits (co_await). `n` above capacity is clamped.
    AcquireOperation acquire(std::uint64_t n) noexcept { return {*this, n}; }

    /// Return `n` permits and wake any waiters that now fit, in FIFO order.
    void release(std::uint64_t n) {
        Waiter wake_local[8];
        std::size_t wake_n = 0;
        std::deque<Waiter> wake_overflow;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            available_ += n;
            while (!waiters_.empty() && available_ >= waiters_.front().need) {
                available_ -= waiters_.front().need;
                Waiter w = waiters_.front();
                waiters_.pop_front();
                if (wake_n < 8)
                    wake_local[wake_n++] = w;
                else
                    wake_overflow.push_back(w);
            }
        }
        for (std::size_t i = 0; i < wake_n; ++i)
            dftracer::utils::schedule_coroutine_resumption_helper(
                wake_local[i].executor, wake_local[i].handle);
        for (const Waiter& w : wake_overflow)
            dftracer::utils::schedule_coroutine_resumption_helper(w.executor,
                                                                  w.handle);
    }

   private:
    struct Waiter {
        std::uint64_t need;
        std::coroutine_handle<> handle;
        dftracer::utils::Executor* executor;
    };

    const std::uint64_t capacity_;
    std::mutex mtx_;
    std::uint64_t available_;
    std::deque<Waiter> waiters_;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_ASYNC_SEMAPHORE_H
