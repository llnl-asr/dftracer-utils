#ifndef DFTRACER_UTILS_CORE_CORO_SPAWN_FUTURE_H
#define DFTRACER_UTILS_CORE_CORO_SPAWN_FUTURE_H

#include <dftracer/utils/core/coro/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace dftracer::utils {
class Executor;
}  // namespace dftracer::utils

namespace dftracer::utils::coro {

/// Lock-free shared state for SpawnFuture<T>.
///
/// Encodes a 3-state machine in a single atomic uintptr_t:
///   0 = EMPTY  -- neither result nor waiter yet
///   1 = DONE   -- result stored, no waiter was registered
///   other      -- WAITING -- value is the waiter's coroutine_handle address
///
/// Completion path (producer):
///   Store result, then exchange(DONE). If prev was a handle address,
///   schedule that handle for resumption via the executor.
///
/// Await path (consumer):
///   CAS(EMPTY → handle_addr). If CAS fails, state is DONE -- don't suspend.
///
/// One heap allocation per typed spawn (shared_ptr<SharedState<T>>).
template <typename T>
struct SharedState {
    static constexpr std::uintptr_t EMPTY = 0;
    static constexpr std::uintptr_t DONE = 1;

    std::atomic<std::uintptr_t> flag{EMPTY};
    std::optional<T> result;
    std::exception_ptr exception;
    Executor* executor{nullptr};

    /// Called by the spawned coroutine on completion.
    void complete(T value) {
        result.emplace(std::move(value));
        auto prev = flag.exchange(DONE, std::memory_order_acq_rel);
        if (prev != EMPTY && prev != DONE) {
            // prev is a waiting coroutine handle address
            auto waiter = std::coroutine_handle<>::from_address(
                reinterpret_cast<void*>(prev));
            schedule_coroutine_resumption_helper(executor, waiter);
        }
    }

    /// Called by the spawned coroutine on exception.
    void complete_with_exception(std::exception_ptr e) {
        exception = std::move(e);
        auto prev = flag.exchange(DONE, std::memory_order_acq_rel);
        if (prev != EMPTY && prev != DONE) {
            auto waiter = std::coroutine_handle<>::from_address(
                reinterpret_cast<void*>(prev));
            schedule_coroutine_resumption_helper(executor, waiter);
        }
    }

    /// Check if result is ready without blocking.
    bool is_done() const {
        return flag.load(std::memory_order_acquire) == DONE;
    }
};

/// Void specialization of SharedState.
template <>
struct SharedState<void> {
    static constexpr std::uintptr_t EMPTY = 0;
    static constexpr std::uintptr_t DONE = 1;

    std::atomic<std::uintptr_t> flag{EMPTY};
    std::exception_ptr exception;
    Executor* executor{nullptr};

    void complete() {
        auto prev = flag.exchange(DONE, std::memory_order_acq_rel);
        if (prev != EMPTY && prev != DONE) {
            auto waiter = std::coroutine_handle<>::from_address(
                reinterpret_cast<void*>(prev));
            schedule_coroutine_resumption_helper(executor, waiter);
        }
    }

    void complete_with_exception(std::exception_ptr e) {
        exception = std::move(e);
        auto prev = flag.exchange(DONE, std::memory_order_acq_rel);
        if (prev != EMPTY && prev != DONE) {
            auto waiter = std::coroutine_handle<>::from_address(
                reinterpret_cast<void*>(prev));
            schedule_coroutine_resumption_helper(executor, waiter);
        }
    }

    bool is_done() const {
        return flag.load(std::memory_order_acquire) == DONE;
    }
};

/// Typed future returned by CoroScope::spawn() for non-void coroutines.
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

    SpawnFuture(SpawnFuture&&) noexcept = default;
    SpawnFuture& operator=(SpawnFuture&&) noexcept = default;
    SpawnFuture(const SpawnFuture&) = delete;
    SpawnFuture& operator=(const SpawnFuture&) = delete;

    // ====================================================================
    // Awaitable interface
    // ====================================================================

    bool await_ready() const noexcept { return state_->is_done(); }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        auto addr = reinterpret_cast<std::uintptr_t>(awaiting.address());
        auto expected = SharedState<T>::EMPTY;
        // Try to register ourselves as the waiter.
        // If CAS fails, the state is already DONE -- don't suspend.
        return state_->flag.compare_exchange_strong(expected, addr,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
    }

    T await_resume() {
        if (state_->exception) {
            auto ex = std::move(state_->exception);
            state_->exception = nullptr;
            std::rethrow_exception(std::move(ex));
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*state_->result);
        }
    }

    /// Check if the spawned coroutine has completed.
    bool is_done() const { return state_->is_done(); }

    /// Detach waiter -- prevents completion from resuming a destroyed handle.
    /// Used by when_any to safely clean up losing wrappers.
    /// If a waiter handle was registered (via await_suspend), it is
    /// extracted and scheduled for deferred destruction since nobody
    /// will resume it.
    void detach() noexcept {
        if (state_) {
            auto prev = state_->flag.exchange(SharedState<T>::DONE,
                                              std::memory_order_acq_rel);
            if (prev != SharedState<T>::EMPTY && prev != SharedState<T>::DONE &&
                state_->executor) {
                // A waiter handle was registered but will never be
                // resumed.  Schedule it for deferred destruction to
                // prevent a frame leak.
                auto stale = std::coroutine_handle<>::from_address(
                    reinterpret_cast<void*>(prev));
                schedule_destroy_helper(state_->executor, stale);
            }
        }
    }

   private:
    std::shared_ptr<SharedState<T>> state_;
};

/// Void specialization of SpawnFuture.
template <>
class SpawnFuture<void> {
   public:
    using result_type = void;

    explicit SpawnFuture(std::shared_ptr<SharedState<void>> state)
        : state_(std::move(state)) {}

    SpawnFuture(SpawnFuture&&) noexcept = default;
    SpawnFuture& operator=(SpawnFuture&&) noexcept = default;
    SpawnFuture(const SpawnFuture&) = delete;
    SpawnFuture& operator=(const SpawnFuture&) = delete;

    bool await_ready() const noexcept { return state_->is_done(); }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        auto addr = reinterpret_cast<std::uintptr_t>(awaiting.address());
        auto expected = SharedState<void>::EMPTY;
        return state_->flag.compare_exchange_strong(expected, addr,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
    }

    void await_resume() {
        if (state_->exception) {
            auto ex = std::move(state_->exception);
            state_->exception = nullptr;
            std::rethrow_exception(std::move(ex));
        }
    }

    bool is_done() const { return state_->is_done(); }

    void detach() noexcept {
        if (state_) {
            auto prev = state_->flag.exchange(SharedState<void>::DONE,
                                              std::memory_order_acq_rel);
            if (prev != SharedState<void>::EMPTY &&
                prev != SharedState<void>::DONE && state_->executor) {
                auto stale = std::coroutine_handle<>::from_address(
                    reinterpret_cast<void*>(prev));
                schedule_destroy_helper(state_->executor, stale);
            }
        }
    }

   private:
    std::shared_ptr<SharedState<void>> state_;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_SPAWN_FUTURE_H
