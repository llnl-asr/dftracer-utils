#ifndef DFTRACER_UTILS_RUNTIME_CORO_SPAWN_FUTURE_H
#define DFTRACER_UTILS_RUNTIME_CORO_SPAWN_FUTURE_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/parked.h>
#include <dftracer/utils/runtime/executor/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace dftracer::utils::coro {

/// Lock-free shared state for SpawnFuture<T>.
///
/// Encodes a 3-state machine in a single atomic uintptr_t:
///   0 = EMPTY  -- neither result nor waiter yet
///   1 = DONE   -- result stored, no waiter was registered
///   2 = CANCELLED -- the waiter was woken early by shutdown
///   other      -- WAITING -- value is the waiter's coroutine_handle address
///
/// Completion path (producer):
///   Store result, then exchange(DONE). If prev was a handle address,
///   schedule that handle for resumption via the executor.
///
/// Await path (consumer):
///   CAS(EMPTY -> handle_addr). If CAS fails, state is DONE -- don't suspend.
///
/// One heap allocation per typed spawn (shared_ptr<SharedState<T>>).
/// `result` is stored only for non-void T; the void case uses a zero-size
/// std::monostate so a single template serves both. The complete() overloads
/// are constrained on void-ness; both share notify_done() for the
/// exchange-and-resume tail.
template <typename T>
struct SharedState {
    static constexpr std::uintptr_t EMPTY = 0;
    static constexpr std::uintptr_t DONE = 1;
    /// A waiter that shutdown woke early; not an address, so no frame matches.
    static constexpr std::uintptr_t CANCELLED = 2;

    std::atomic<std::uintptr_t> flag{EMPTY};
    [[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate,
                                             std::optional<T>> result;
    std::exception_ptr exception;
    Executor* executor{nullptr};
    /// Cancellation token of the spawned child, set by SpawnFuture::cancel().
    std::shared_ptr<std::atomic<bool>> cancel_token;

    /// Called by the spawned coroutine on completion (non-void result).
    template <typename U = T>
        requires(!std::is_void_v<U>)
    void complete(U value) {
        result.emplace(std::move(value));
        notify_done();
    }

    /// Called by the spawned coroutine on completion (void result).
    template <typename U = T>
        requires std::is_void_v<U>
    void complete() {
        notify_done();
    }

    /// Called by the spawned coroutine on exception.
    void complete_with_exception(std::exception_ptr e) {
        exception = std::move(e);
        notify_done();
    }

    /// Check if result is ready without blocking.
    bool is_done() const {
        return flag.load(std::memory_order_acquire) == DONE;
    }

   private:
    /// Transition to DONE and resume a registered waiter, if any.
    void notify_done() {
        auto prev = flag.exchange(DONE, std::memory_order_acq_rel);
        if (prev != EMPTY && prev != DONE && prev != CANCELLED) {
            // prev is a waiting coroutine handle address.
            auto waiter = std::coroutine_handle<>::from_address(
                reinterpret_cast<void*>(prev));
            schedule_coroutine_resumption_helper(executor, waiter);
        }
    }
};

/// Future returned by CoroScope::spawn() for both void and typed coroutines
/// (await_resume yields void when T is void).
///
/// Awaitable: co_await on a SpawnFuture<T> suspends the caller until
/// the spawned coroutine completes, then returns the typed result.
///
/// Usage:
/// @code
/// SpawnFuture<int> future = scope.spawn([](CoroScope& s) -> CoroTask<int> {
///     co_return 42;
/// });
/// int result = co_await future;  // suspends until spawn completes
/// @endcode
template <typename T>
class SpawnFuture {
   public:
    using result_type = T;

    explicit SpawnFuture(std::shared_ptr<SharedState<T>> state)
        : state_(std::move(state)) {}

    SpawnFuture(SpawnFuture&& other) noexcept
        : state_(std::move(other.state_)) {}
    SpawnFuture& operator=(SpawnFuture&& other) noexcept {
        state_ = std::move(other.state_);
        return *this;
    }
    SpawnFuture(const SpawnFuture&) = delete;
    SpawnFuture& operator=(const SpawnFuture&) = delete;

    ~SpawnFuture() {
        if (park_) executor_->unpark(park_);
    }

    // ====================================================================
    // Awaitable interface
    // ====================================================================

    bool await_ready() const noexcept { return state_->is_done(); }

    /// co_await throws a cancelled error (see coro::is_cancelled) when the
    /// executor shuts down while the spawn is still running; the spawn is not
    /// stopped by that, and its scope still joins it.
    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle_ = awaiting;
        executor_ = resume_executor_for(state_->executor);
        // Registered before the flag publishes the waiter: once it does, the
        // spawn may resume the frame on another thread at any moment.
        if (executor_) {
            park_ = executor_->park(this, &SpawnFuture::cancel_thunk);
        }
        auto addr = reinterpret_cast<std::uintptr_t>(awaiting.address());
        auto expected = SharedState<T>::EMPTY;
        // If the CAS fails the state is already DONE: do not suspend.
        if (state_->flag.compare_exchange_strong(expected, addr,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return true;
        }
        if (park_) executor_->unpark(park_);
        park_ = nullptr;
        return false;
    }

    static void cancel_thunk(void* self) noexcept {
        static_cast<SpawnFuture*>(self)->cancel_parked();
    }

    void cancel_parked() noexcept {
        auto expected = reinterpret_cast<std::uintptr_t>(handle_.address());
        if (state_->flag.compare_exchange_strong(
                expected, SharedState<T>::CANCELLED, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            cancelled_ = true;
            schedule_coroutine_resumption_helper(executor_, handle_);
        }
    }

    T await_resume() {
        if (park_) {
            executor_->unpark(park_);
            park_ = nullptr;
        }
        if (cancelled_) throw_cancelled("future wait cancelled");
        if (state_->exception) {
            rethrow_and_clear(state_->exception);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*state_->result);
        }
    }

    /// Check if the spawned coroutine has completed.
    bool is_done() const { return state_->is_done(); }

    /// Ask the spawned coroutine to stop. Cooperative: the child sees it
    /// through CoroScope::is_cancellation_requested(). Thread-safe, idempotent.
    void cancel() noexcept {
        if (state_ && state_->cancel_token) {
            state_->cancel_token->store(true, std::memory_order_release);
        }
    }

   private:
    std::shared_ptr<SharedState<T>> state_;
    std::coroutine_handle<> handle_{};
    Executor* executor_ = nullptr;
    ParkNode* park_ = nullptr;
    bool cancelled_ = false;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_SPAWN_FUTURE_H
