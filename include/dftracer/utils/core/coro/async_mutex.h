#ifndef DFTRACER_UTILS_CORE_CORO_ASYNC_MUTEX_H
#define DFTRACER_UTILS_CORE_CORO_ASYNC_MUTEX_H

// Lock-free async mutex for C++20 coroutines.
//
// Adapted from the lock-free async_mutex design in cppcoro by Lewis Baker.
// Original: https://github.com/lewissbaker/cppcoro (MIT License)
//
// This is an independent implementation adapted for dftracer-utils,
// using C++20 std::coroutine_handle and project conventions.
// The core algorithm (atomic state with intrusive waiter stack,
// LIFO-to-FIFO reversal on unlock) follows cppcoro's design.

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>

namespace dftracer::utils::coro {

class AsyncMutexLockOperation;

/**
 * @brief Lock-free async mutex for coroutines.
 *
 * Ownership is not tied to any thread. A coroutine holding the lock can
 * migrate across threads freely. Waiting coroutines suspend without
 * blocking the thread and are resumed in approximate FIFO order.
 *
 * Usage:
 * @code
 * AsyncMutex mutex;
 *
 * co_await mutex.lock();
 * co_await writer.write_line(data);
 * mutex.unlock();
 *
 * // Or with scoped lock:
 * {
 *     auto guard = co_await mutex.scoped_lock();
 *     co_await writer.write_line(data);
 * }
 * @endcode
 */
class AsyncMutex {
   public:
    AsyncMutex() noexcept : state_(NOT_LOCKED), waiters_(nullptr) {}

    ~AsyncMutex() {
        assert(state_.load(std::memory_order_relaxed) == NOT_LOCKED ||
               state_.load(std::memory_order_relaxed) == LOCKED_NO_WAITERS);
        assert(waiters_ == nullptr);
    }

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;

    bool try_lock() noexcept {
        auto old = NOT_LOCKED;
        return state_.compare_exchange_strong(old, LOCKED_NO_WAITERS,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed);
    }

    AsyncMutexLockOperation lock() noexcept;

    void unlock();

   private:
    friend class AsyncMutexLockOperation;

    // not_locked = 1 (not 0, so that 0 can mean locked_no_waiters
    // and any other value is a pointer to a waiter list)
    static constexpr std::uintptr_t NOT_LOCKED = 1;
    static constexpr std::uintptr_t LOCKED_NO_WAITERS = 0;

    // Three states:
    // - NOT_LOCKED (1)
    // - LOCKED_NO_WAITERS (0)
    // - pointer to head of waiter stack (LIFO push order)
    std::atomic<std::uintptr_t> state_;

    // FIFO list of waiters being serviced. Filled by reversing
    // the LIFO stack from state_ on unlock.
    AsyncMutexLockOperation* waiters_;
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
 * If contended, suspends and pushes onto the waiter stack.
 */
class AsyncMutexLockOperation {
   public:
    explicit AsyncMutexLockOperation(AsyncMutex& mutex) noexcept
        : mutex_(mutex) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        awaiter_ = awaiter;

        std::uintptr_t old = mutex_.state_.load(std::memory_order_acquire);
        while (true) {
            if (old == AsyncMutex::NOT_LOCKED) {
                if (mutex_.state_.compare_exchange_weak(
                        old, AsyncMutex::LOCKED_NO_WAITERS,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    return false;  // Acquired, don't suspend
                }
            } else {
                next_ = reinterpret_cast<AsyncMutexLockOperation*>(old);
                if (mutex_.state_.compare_exchange_weak(
                        old, reinterpret_cast<std::uintptr_t>(this),
                        std::memory_order_release, std::memory_order_relaxed)) {
                    return true;  // Queued, suspend
                }
            }
        }
    }

    void await_resume() const noexcept {}

   private:
    friend class AsyncMutex;

    AsyncMutex& mutex_;
    AsyncMutexLockOperation* next_ = nullptr;
    std::coroutine_handle<> awaiter_;
};

// -- Inline definitions --

inline AsyncMutexLockOperation AsyncMutex::lock() noexcept {
    return AsyncMutexLockOperation{*this};
}

inline void AsyncMutex::unlock() {
    assert(state_.load(std::memory_order_relaxed) != NOT_LOCKED);

    auto* head = waiters_;
    if (head == nullptr) {
        auto old = LOCKED_NO_WAITERS;
        if (state_.compare_exchange_strong(old, NOT_LOCKED,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
            return;
        }

        // New waiters arrived on the atomic stack. Detach and reverse to FIFO.
        old = state_.exchange(LOCKED_NO_WAITERS, std::memory_order_acquire);
        assert(old != LOCKED_NO_WAITERS && old != NOT_LOCKED);

        auto* next = reinterpret_cast<AsyncMutexLockOperation*>(old);
        do {
            auto* temp = next->next_;
            next->next_ = head;
            head = next;
            next = temp;
        } while (next != nullptr);
    }

    assert(head != nullptr);

    waiters_ = head->next_;
    head->awaiter_.resume();
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_ASYNC_MUTEX_H
