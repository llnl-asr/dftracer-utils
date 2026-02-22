#ifndef DFTRACER_UTILS_CORE_CORO_WHEN_ALL_H
#define DFTRACER_UTILS_CORE_CORO_WHEN_ALL_H

#include <dftracer/utils/core/coro/resumption_helper.h>
#include <dftracer/utils/core/coro/task.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace dftracer::utils::coro {

// ============================================================================
// WhenAllVectorAwaitable - Homogeneous awaitable types (vector)
// Heap-allocated state pattern
// ============================================================================

/**
 * Shared state for WhenAllVectorAwaitable
 * Heap-allocated to ensure lifetime extends beyond await_suspend
 */
template <typename Awaitable>
struct WhenAllVectorState {
    std::vector<Awaitable> awaitables_;
    std::vector<typename Awaitable::result_type> results_;
    std::exception_ptr exception_;
    std::atomic<bool> has_exception_{false};
    std::atomic<std::size_t> completed_count_{0};
    std::coroutine_handle<> awaiting_coroutine_;
    std::vector<CoroTask<void>> wrapper_coros_;
    std::size_t total_;
    Executor* executor_{nullptr};

    // Single-atomic coordination between await_suspend and on_one_complete.
    // Uses fetch_or(acq_rel) on a bitmask -- the total modification order on
    // one atomic guarantees exactly one side sees the other's bit, eliminating
    // the store-buffer (SB) reordering hazard that two independent atomics
    // with seq_cst were guarding against.
    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> sync_state_{0};

    explicit WhenAllVectorState(std::vector<Awaitable> awaitables)
        : awaitables_(std::move(awaitables)),
          results_(awaitables_.size()),
          total_(awaitables_.size()) {
        wrapper_coros_.reserve(total_);
    }

    void on_one_complete() {
        std::size_t count =
            completed_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == total_) {
            auto prev =
                sync_state_.fetch_or(BIT_COMPLETED, std::memory_order_acq_rel);
            if (prev & BIT_SUSPENDED) {
                if (awaiting_coroutine_ && !awaiting_coroutine_.done()) {
                    if (executor_) {
                        schedule_coroutine_resumption_helper(
                            executor_, awaiting_coroutine_);
                    } else {
                        awaiting_coroutine_.resume();
                    }
                }
            }
        }
    }

    void on_exception(std::exception_ptr e) {
        bool expected = false;
        if (has_exception_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
            exception_ = e;
        }
        on_one_complete();
    }

    void mark_suspended_and_check_completion() {
        auto prev =
            sync_state_.fetch_or(BIT_SUSPENDED, std::memory_order_acq_rel);
        if (prev & BIT_COMPLETED) {
            if (awaiting_coroutine_ && !awaiting_coroutine_.done()) {
                if (executor_) {
                    schedule_coroutine_resumption_helper(executor_,
                                                         awaiting_coroutine_);
                } else {
                    awaiting_coroutine_.resume();
                }
            }
        }
    }
};

/**
 * WhenAllVectorAwaitable - Lightweight awaitable that wraps shared state
 *
 * This is a thin handle that points to heap-allocated state.
 * The state is kept alive by shared_ptr until all tasks complete.
 *
 * Usage:
 * @code
 * CoroTask<Data> read_ops;
 * for (int i = 0; i < 16000; i++) {
 *     read_ops.push_back(ctx.blocking([i]() { return read_chunk(i); }));
 * }
 * auto results = co_await when_all(read_ops);  // Vector<Data>
 * @endcode
 */
template <typename Awaitable>
class WhenAllVectorAwaitable {
   private:
    std::shared_ptr<WhenAllVectorState<Awaitable>> state_;

   public:
    using result_type = std::vector<typename Awaitable::result_type>;

    explicit WhenAllVectorAwaitable(std::vector<Awaitable> awaitables)
        : state_(std::make_shared<WhenAllVectorState<Awaitable>>(
              std::move(awaitables))) {}

    bool await_ready() {
        return std::all_of(state_->awaitables_.begin(),
                           state_->awaitables_.end(),
                           [](auto& a) { return a.await_ready(); });
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine_ = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor_ = root->get_executor();
        }

        if (state_->total_ == 0) {
            return false;
        }

        for (std::size_t i = 0; i < state_->total_; i++) {
            launch_wrapper(i);
        }

        // Check if all completed synchronously
        if (state_->completed_count_.load(std::memory_order_acquire) ==
            state_->total_) {
            return false;  // Don't suspend
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async_ = true;
        }

        // We will suspend - mark it and double-check for completion.
        // Do this last: completion may schedule/resume and destroy this frame.
        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
        return std::move(state_->results_);
    }

   private:
    void launch_wrapper(std::size_t i) {
        auto state = state_;

        auto wrapper_coro = [](std::shared_ptr<WhenAllVectorState<Awaitable>> s,
                               std::size_t index) -> CoroTask<void> {
            try {
                s->results_[index] = co_await s->awaitables_[index];
                s->on_one_complete();
            } catch (...) {
                s->on_exception(std::current_exception());
            }
        }(state, i);

        state_->wrapper_coros_.push_back(std::move(wrapper_coro));
        // Start the lazy coroutine - it will suspend at its first co_await
        state_->wrapper_coros_.back().handle().resume();
    }
};

/**
 * when_all - Wait for all awaitables to complete (vector version)
 *
 * @param awaitables Vector of awaitables
 * @return Awaitable that returns vector of results
 *
 * Usage:
 * @code
 * std::vector<TaskFuture<int>> futures;
 * for (int i = 0; i < 100; i++) {
 *     futures.push_back(ctx.spawn([]() -> Task<int> {
 *         co_return compute();
 *     }));
 * }
 * auto results = co_await when_all(futures);
 * @endcode
 */
template <typename Awaitable>
auto when_all(std::vector<Awaitable> awaitables) {
    return WhenAllVectorAwaitable<Awaitable>(std::move(awaitables));
}

// ============================================================================
// Helper: when_all with initializer_list
// ============================================================================

/**
 * when_all - Wait for all awaitables to complete (initializer_list version)
 *
 * Usage:
 * @code
 * auto results = co_await when_all({future1, future2, future3});
 * @endcode
 */
template <typename Awaitable>
auto when_all(std::initializer_list<Awaitable> awaitables) {
    return WhenAllVectorAwaitable<Awaitable>(
        std::vector<Awaitable>(awaitables.begin(), awaitables.end()));
}

// ============================================================================
// Specialization for void result types
// ============================================================================

template <typename Awaitable>
    requires(std::is_void_v<typename Awaitable::result_type>)
struct WhenAllVectorState<Awaitable> {
    std::vector<Awaitable> awaitables_;
    std::exception_ptr exception_;
    std::atomic<bool> has_exception_{false};
    std::atomic<std::size_t> completed_count_{0};
    std::coroutine_handle<> awaiting_coroutine_;
    std::vector<CoroTask<void>> wrapper_coros_;
    std::size_t total_;
    Executor* executor_{nullptr};

    // Single-atomic coordination between await_suspend and on_one_complete.
    // Uses fetch_or(acq_rel) on a bitmask -- the total modification order on
    // one atomic guarantees exactly one side sees the other's bit, eliminating
    // the store-buffer (SB) reordering hazard that two independent atomics
    // with seq_cst were guarding against.
    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> sync_state_{0};

    explicit WhenAllVectorState(std::vector<Awaitable> awaitables)
        : awaitables_(std::move(awaitables)), total_(awaitables_.size()) {
        wrapper_coros_.reserve(total_);
    }

    void on_one_complete() {
        std::size_t count =
            completed_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == total_) {
            auto prev =
                sync_state_.fetch_or(BIT_COMPLETED, std::memory_order_acq_rel);
            if (prev & BIT_SUSPENDED) {
                if (awaiting_coroutine_ && !awaiting_coroutine_.done()) {
                    if (executor_) {
                        schedule_coroutine_resumption_helper(
                            executor_, awaiting_coroutine_);
                    } else {
                        awaiting_coroutine_.resume();
                    }
                }
            }
        }
    }

    void on_exception(std::exception_ptr ex) {
        bool expected = false;
        if (has_exception_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
            exception_ = ex;
        }
        on_one_complete();
    }

    // Called by await_suspend after deciding to suspend but before returning
    void mark_suspended_and_check_completion() {
        auto prev =
            sync_state_.fetch_or(BIT_SUSPENDED, std::memory_order_acq_rel);
        if (prev & BIT_COMPLETED) {
            if (awaiting_coroutine_ && !awaiting_coroutine_.done()) {
                if (executor_) {
                    schedule_coroutine_resumption_helper(executor_,
                                                         awaiting_coroutine_);
                } else {
                    awaiting_coroutine_.resume();
                }
            }
        }
    }
};

template <typename Awaitable>
    requires(std::is_void_v<typename Awaitable::result_type>)
class WhenAllVectorAwaitable<Awaitable> {
   private:
    std::shared_ptr<WhenAllVectorState<Awaitable>> state_;

   public:
    using result_type = void;

    explicit WhenAllVectorAwaitable(std::vector<Awaitable> awaitables)
        : state_(std::make_shared<WhenAllVectorState<Awaitable>>(
              std::move(awaitables))) {}

    bool await_ready() {
        return std::all_of(state_->awaitables_.begin(),
                           state_->awaitables_.end(),
                           [](auto& a) { return a.await_ready(); });
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine_ = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor_ = root->get_executor();
        }

        if (state_->total_ == 0) {
            return false;
        }

        for (std::size_t i = 0; i < state_->total_; ++i) {
            launch_wrapper(i);
        }

        // Check if all tasks completed synchronously during launch_wrapper
        if (state_->completed_count_.load(std::memory_order_acquire) ==
            state_->total_) {
            return false;  // Don't suspend
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async_ = true;
        }

        // We will suspend - mark it and double-check for completion.
        // Do this last: completion may schedule/resume and destroy this frame.
        state_->mark_suspended_and_check_completion();

        return true;
    }

    void await_resume() {
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
    }

   private:
    void launch_wrapper(std::size_t i) {
        auto state = state_;

        auto wrapper_coro = [](std::shared_ptr<WhenAllVectorState<Awaitable>> s,
                               std::size_t index) -> CoroTask<void> {
            try {
                co_await s->awaitables_[index];
                s->on_one_complete();
            } catch (...) {
                s->on_exception(std::current_exception());
            }
        }(state, i);

        state_->wrapper_coros_.push_back(std::move(wrapper_coro));
        // Start the lazy coroutine - it will suspend at its first co_await
        state_->wrapper_coros_.back().handle().resume();
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_WHEN_ALL_H
