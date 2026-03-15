#ifndef DFTRACER_UTILS_CORE_CORO_WHEN_ALL_H
#define DFTRACER_UTILS_CORE_CORO_WHEN_ALL_H

#include <dftracer/utils/core/coro/resumption_helper.h>
#include <dftracer/utils/core/coro/task.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace dftracer::utils::coro {

// ============================================================================
// FireAndForget - self-destroying coroutine for when_all wrappers
// ============================================================================

/// Coroutine that destroys its own frame on completion.
struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

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
    std::size_t total_;
    Executor* executor_{nullptr};

    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> sync_state_{0};

    explicit WhenAllVectorState(std::vector<Awaitable> awaitables)
        : awaitables_(std::move(awaitables)),
          results_(awaitables_.size()),
          total_(awaitables_.size()) {}

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
 *     read_ops.push_back(io::async_read(fds[i], bufs[i], lens[i]));
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

    // Always return false: wrapper coroutines are launched in
    // await_suspend, so we must always enter it.  If all sub-awaitables
    // complete synchronously, await_suspend returns false (no suspend).
    bool await_ready() { return false; }

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
        [](std::shared_ptr<WhenAllVectorState<Awaitable>> s,
           std::size_t index) -> FireAndForget {
            try {
                s->results_[index] = co_await std::move(s->awaitables_[index]);
                s->on_one_complete();
            } catch (...) {
                s->on_exception(std::current_exception());
            }
        }(state_, i);
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
    std::size_t total_;
    Executor* executor_{nullptr};

    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> sync_state_{0};

    explicit WhenAllVectorState(std::vector<Awaitable> awaitables)
        : awaitables_(std::move(awaitables)), total_(awaitables_.size()) {}

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

    // Always return false: wrapper coroutines are launched in
    // await_suspend, so we must always enter it.  If all sub-awaitables
    // complete synchronously, await_suspend returns false (no suspend).
    bool await_ready() { return false; }

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
        [](std::shared_ptr<WhenAllVectorState<Awaitable>> s,
           std::size_t index) -> FireAndForget {
            try {
                co_await std::move(s->awaitables_[index]);
                s->on_one_complete();
            } catch (...) {
                s->on_exception(std::current_exception());
            }
        }(state_, i);
    }
};

// ============================================================================
// when_all_result_t - maps void result to std::monostate for tuple storage
// ============================================================================

template <typename T>
using when_all_result_t =
    std::conditional_t<std::is_void_v<T>, std::monostate, T>;

// ============================================================================
// WhenAllTupleAwaitable - Heterogeneous awaitable types (variadic/tuple)
// Heap-allocated state pattern, mirrors WhenAllVectorState/Awaitable exactly.
// ============================================================================

/**
 * Shared state for WhenAllTupleAwaitable.
 * Heap-allocated so lifetime extends beyond await_suspend.
 */
template <typename... Awaitables>
struct WhenAllTupleState {
    static constexpr std::size_t total_ = sizeof...(Awaitables);

    std::tuple<Awaitables...> awaitables_;
    std::tuple<
        std::optional<when_all_result_t<typename Awaitables::result_type>>...>
        results_;
    std::exception_ptr exception_;
    std::atomic<bool> has_exception_{false};
    std::atomic<std::size_t> completed_count_{0};
    std::coroutine_handle<> awaiting_coroutine_;
    Executor* executor_{nullptr};

    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> sync_state_{0};

    explicit WhenAllTupleState(Awaitables&&... awaitables)
        : awaitables_(std::forward<Awaitables>(awaitables)...) {}

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
 * WhenAllTupleAwaitable - Lightweight awaitable wrapping heap-allocated state.
 *
 * Accepts heterogeneous awaitable types; returns std::tuple of their results.
 * Void result types are mapped to std::monostate in the tuple.
 *
 * Usage:
 * @code
 * auto [a, b, c] = co_await when_all(task_int(), task_str(), task_float());
 * @endcode
 */
template <typename... Awaitables>
class WhenAllTupleAwaitable {
   public:
    using result_type =
        std::tuple<when_all_result_t<typename Awaitables::result_type>...>;

    explicit WhenAllTupleAwaitable(Awaitables&&... awaitables)
        : state_(std::make_shared<WhenAllTupleState<Awaitables...>>(
              std::forward<Awaitables>(awaitables)...)) {}

    // Always return false: wrapper coroutines are launched in
    // await_suspend, so we must always enter it.  If all sub-awaitables
    // complete synchronously, await_suspend returns false (no suspend).
    bool await_ready() { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine_ = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor_ = root->get_executor();
        }

        if constexpr (WhenAllTupleState<Awaitables...>::total_ == 0) {
            return false;
        }

        launch_all(std::index_sequence_for<Awaitables...>{});

        // Check if all completed synchronously during launch
        if (state_->completed_count_.load(std::memory_order_acquire) ==
            WhenAllTupleState<Awaitables...>::total_) {
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
        return build_result(std::index_sequence_for<Awaitables...>{});
    }

   private:
    std::shared_ptr<WhenAllTupleState<Awaitables...>> state_;

    template <std::size_t... Is>
    result_type build_result(std::index_sequence<Is...>) {
        return result_type{std::move(*std::get<Is>(state_->results_))...};
    }

    template <std::size_t I>
    void launch_one() {
        using A = std::tuple_element_t<I, std::tuple<Awaitables...>>;
        using R = typename A::result_type;

        [](std::shared_ptr<WhenAllTupleState<Awaitables...>> s)
            -> FireAndForget {
            try {
                if constexpr (std::is_void_v<R>) {
                    co_await std::move(std::get<I>(s->awaitables_));
                    std::get<I>(s->results_).emplace(std::monostate{});
                } else {
                    std::get<I>(s->results_)
                        .emplace(
                            co_await std::move(std::get<I>(s->awaitables_)));
                }
                s->on_one_complete();
            } catch (...) {
                s->on_exception(std::current_exception());
            }
        }(state_);
    }

    // Expand index sequence to launch all wrapper coroutines.
    template <std::size_t... Is>
    void launch_all(std::index_sequence<Is...>) {
        (launch_one<Is>(), ...);
    }
};

/**
 * when_all - Wait for all awaitables to complete (variadic/tuple version)
 *
 * Accepts 2+ heterogeneous awaitable types and returns a std::tuple of
 * their results. Void result types appear as std::monostate in the tuple.
 *
 * The requires(sizeof...(Awaitables) >= 2) constraint prevents ambiguity
 * with the single-argument vector overload.
 *
 * Usage:
 * @code
 * auto [n, s] = co_await when_all(task_returning_int(), task_returning_str());
 * @endcode
 */
template <typename... Awaitables>
    requires(sizeof...(Awaitables) >= 2)
auto when_all(Awaitables&&... awaitables) {
    return WhenAllTupleAwaitable<std::decay_t<Awaitables>...>(
        std::forward<Awaitables>(awaitables)...);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_WHEN_ALL_H
