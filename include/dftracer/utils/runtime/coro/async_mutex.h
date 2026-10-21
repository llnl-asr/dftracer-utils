#ifndef DFTRACER_UTILS_RUNTIME_CORO_ASYNC_MUTEX_H
#define DFTRACER_UTILS_RUNTIME_CORO_ASYNC_MUTEX_H

#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/parked.h>
#include <dftracer/utils/runtime/executor/resumption_helper.h>

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::coro {

class AsyncMutexLockOperation;

/**
 * @brief Async mutex for coroutines.
 *
 * Ownership is not tied to any thread. A coroutine holding the lock can
 * migrate across threads freely. Waiting coroutines suspend without
 * blocking the thread and are resumed in FIFO order. Locking and unlocking
 * an uncontended mutex is one atomic operation; only a contended lock
 * takes the internal queue lock, which is never held across a suspension.
 *
 * A coroutine waiting for the lock when its executor shuts down is resumed
 * with a cancelled error (see coro::is_cancelled), without the lock.
 *
 * Usage:
 * @code
 * AsyncMutex mutex;
 *
 * co_await mutex.lock();
 * co_await writer.write_line(data);
 * mutex.unlock();
 * @endcode
 */
class AsyncMutex {
   public:
    AsyncMutex() noexcept = default;

    ~AsyncMutex() {
        assert(state_.load(std::memory_order_relaxed) != LOCKED_WAITERS);
        assert(head_ == nullptr);
    }

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;

    AsyncMutexLockOperation lock() noexcept;

    [[gnu::always_inline]] void unlock();

   private:
    friend class AsyncMutexLockOperation;

    static constexpr std::uint32_t FREE = 0;
    static constexpr std::uint32_t LOCKED = 1;
    /// Locked and the queue is, or is about to be, non-empty. Changes only
    /// with queue_mutex_ held, except the FREE-to-LOCKED and LOCKED-to-FREE
    /// steps of an uncontended lock and unlock.
    static constexpr std::uint32_t LOCKED_WAITERS = 2;

    /// The unlock that finds waiters. Out of line, and the only frame a chain
    /// of unlocks adds per waiter, because unlock() is inlined into its caller.
    [[gnu::noinline]] inline void unlock_contended();

    void push_locked(AsyncMutexLockOperation* op) noexcept;
    void remove_locked(AsyncMutexLockOperation* op) noexcept;

    std::atomic<std::uint32_t> state_{FREE};
    std::mutex queue_mutex_;
    AsyncMutexLockOperation* head_ = nullptr;
    AsyncMutexLockOperation* tail_ = nullptr;
};

/**
 * @brief RAII lock guard returned by AsyncMutex::scoped_lock().
 */
class AsyncMutexGuard {
   public:
    explicit AsyncMutexGuard(AsyncMutex& mutex) noexcept : mutex_(&mutex) {}

    AsyncMutexGuard(AsyncMutexGuard&& other) noexcept : mutex_(other.mutex_) {
        other.mutex_ = nullptr;
    }

    AsyncMutexGuard(const AsyncMutexGuard&) = delete;
    AsyncMutexGuard& operator=(const AsyncMutexGuard&) = delete;

    ~AsyncMutexGuard() {
        if (mutex_) mutex_->unlock();
    }

   private:
    AsyncMutex* mutex_;
};

/**
 * @brief Awaitable returned by AsyncMutex::lock().
 *
 * On co_await: if mutex is free, acquires immediately (no suspend).
 * If contended, suspends and joins the queue.
 */
class AsyncMutexLockOperation {
   public:
    explicit AsyncMutexLockOperation(AsyncMutex& mutex) noexcept
        : mutex_(mutex) {}

    ~AsyncMutexLockOperation() {
        if (park_) executor_->unpark(park_);
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        std::uint32_t s = AsyncMutex::FREE;
        if (mutex_.state_.compare_exchange_strong(s, AsyncMutex::LOCKED,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
            return false;
        }
        return queue(awaiter);
    }

    /// The contended case, kept out of line so the uncontended lock stays a
    /// few inline instructions.
    [[gnu::noinline]] bool queue(std::coroutine_handle<> awaiter) noexcept {
        std::uint32_t s;
        awaiter_ = awaiter;
        executor_ = dftracer::utils::resume_executor_for(nullptr);
        bool acquired = false;
        {
            std::lock_guard<std::mutex> lock(mutex_.queue_mutex_);
            // The caller found it locked, but a slow unlock may have freed it
            // since.
            s = mutex_.state_.load(std::memory_order_relaxed);
            for (;;) {
                if (s == AsyncMutex::FREE) {
                    if (mutex_.state_.compare_exchange_strong(
                            s, AsyncMutex::LOCKED, std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        acquired = true;
                        break;
                    }
                } else if (s == AsyncMutex::LOCKED_WAITERS ||
                           mutex_.state_.compare_exchange_strong(
                               s, AsyncMutex::LOCKED_WAITERS,
                               std::memory_order_relaxed,
                               std::memory_order_relaxed)) {
                    break;
                }
            }
            if (acquired) return false;
            mutex_.push_locked(this);
            if (executor_) {
                park_ = executor_->park(this,
                                        &AsyncMutexLockOperation::cancel_thunk);
            }
        }
        return true;
    }

    void await_resume() {
        if (park_) [[unlikely]] {
            const bool cancelled = park_->cancelled;
            executor_->unpark(park_);
            park_ = nullptr;
            if (cancelled) throw_cancelled("mutex lock cancelled");
        }
    }

    static void cancel_thunk(void* self) noexcept {
        static_cast<AsyncMutexLockOperation*>(self)->cancel_parked();
    }

    void cancel_parked() noexcept {
        {
            std::unique_lock<std::mutex> lock(mutex_.queue_mutex_,
                                              std::try_to_lock);
            if (!lock || !queued_) return;
            mutex_.remove_locked(this);
            park_->cancelled = true;
        }
        dftracer::utils::schedule_coroutine_resumption_helper(executor_,
                                                              awaiter_);
    }

   private:
    friend class AsyncMutex;

    AsyncMutex& mutex_;
    AsyncMutexLockOperation* prev_ = nullptr;
    AsyncMutexLockOperation* next_ = nullptr;
    dftracer::utils::Executor* executor_ = nullptr;
    std::coroutine_handle<> awaiter_;
    ParkNode* park_ = nullptr;
    bool queued_ = false;
};

// -- Inline definitions --

inline AsyncMutexLockOperation AsyncMutex::lock() noexcept {
    return AsyncMutexLockOperation{*this};
}

inline void AsyncMutex::push_locked(AsyncMutexLockOperation* op) noexcept {
    op->prev_ = tail_;
    op->next_ = nullptr;
    if (tail_) {
        tail_->next_ = op;
    } else {
        head_ = op;
    }
    tail_ = op;
    op->queued_ = true;
}

inline void AsyncMutex::remove_locked(AsyncMutexLockOperation* op) noexcept {
    if (op->prev_) {
        op->prev_->next_ = op->next_;
    } else {
        head_ = op->next_;
    }
    if (op->next_) {
        op->next_->prev_ = op->prev_;
    } else {
        tail_ = op->prev_;
    }
    op->prev_ = op->next_ = nullptr;
    op->queued_ = false;
    if (head_ == nullptr) {
        std::uint32_t s = LOCKED_WAITERS;
        state_.compare_exchange_strong(s, LOCKED, std::memory_order_relaxed,
                                       std::memory_order_relaxed);
    }
}

[[gnu::always_inline]] inline void AsyncMutex::unlock() {
    assert(state_.load(std::memory_order_relaxed) != FREE);

    std::uint32_t s = LOCKED;
    if (state_.compare_exchange_strong(s, FREE, std::memory_order_release,
                                       std::memory_order_relaxed)) {
        return;
    }
    unlock_contended();
}

void AsyncMutex::unlock_contended() {
    AsyncMutexLockOperation* next;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        next = head_;
        if (next == nullptr) {
            state_.store(FREE, std::memory_order_release);
            return;
        }
        // The lock passes to `next`; the state keeps saying locked.
        remove_locked(next);
    }

    // The waiter may belong to a RunLoop on another thread, which only that
    // thread can drive.
    if (next->executor_ != nullptr &&
        next->executor_ != dftracer::utils::Executor::current()) {
        dftracer::utils::schedule_coroutine_resumption_helper(next->executor_,
                                                              next->awaiter_);
        return;
    }
    memory::Scope scope{memory::Manager::get().general()};
    next->awaiter_.resume();
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_ASYNC_MUTEX_H
