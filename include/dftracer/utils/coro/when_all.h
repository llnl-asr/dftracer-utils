// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_WHEN_ALL_H
#define DFTRACER_UTILS_CORO_WHEN_ALL_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/detail/completion_state.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/detail/pool_resume.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <tuple>
#include <utility>
#include <variant>

namespace dftracer::utils::coro {

// ============================================================================
// FireAndForget - self-destroying coroutine for when_all wrappers
// ============================================================================

/// Coroutine that destroys its own frame on completion.
struct FireAndForget {
    struct promise_type : detail::FrameAllocation {
        detail::RunContext context;

        template <typename U>
        auto await_transform(U&& awaitable) noexcept {
            return detail::in_pool(std::forward<U>(awaitable), context);
        }

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
 * Shared state for @ref WhenAllVectorAwaitable
 * Heap-allocated to ensure lifetime extends beyond await_suspend
 */
template <typename Awaitable>
struct WhenAllVectorState : detail::WhenAllCompletionState {
    dstd::Vector<Awaitable> awaitables;
    /// dstd::Vector<bool> packs neighbours into one byte, so two wrappers
    /// writing them would race. Bool results take one byte each.
    using Stored = std::conditional_t<
        std::is_same_v<typename Awaitable::result_type, bool>, unsigned char,
        typename Awaitable::result_type>;
    dstd::Vector<Stored> results;
    detail::ChildClaims claims;

    explicit WhenAllVectorState(dstd::Vector<Awaitable> items)
        : awaitables(std::move(items)), results(awaitables.size()) {
        total = awaitables.size();
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
    dstd::SharedPtr<WhenAllVectorState<Awaitable>> state_;

   public:
    using result_type = dstd::Vector<typename Awaitable::result_type>;

    explicit WhenAllVectorAwaitable(dstd::Vector<Awaitable> awaitables)
        : state_(dstd::make_shared<WhenAllVectorState<Awaitable>>(
              std::move(awaitables))) {}

    /// Always return false: wrapper coroutines are launched in
    /// await_suspend, so we must always enter it.  If all sub-awaitables
    /// complete synchronously, await_suspend returns false (no suspend).
    bool await_ready() { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor = root->get_executor();
        }

        if (state_->total == 0) {
            return false;
        }

        while (!state_->claims.exhausted(state_->total)) launch_wrapper();

        // Check if all completed synchronously
        if (state_->completed_count.load(std::memory_order_acquire) ==
            state_->total) {
            return false;  // Don't suspend
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async = true;
        }

        // We will suspend - mark it and double-check for completion.
        // Do this last: completion may schedule/resume and destroy this frame.
        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        state_->finish_wait();
        if (state_->exception) {
            common::rethrow_and_clear(state_->exception);
        }
        if constexpr (std::is_same_v<typename Awaitable::result_type, bool>) {
            return result_type(state_->results.begin(), state_->results.end());
        } else {
            return std::move(state_->results);
        }
    }

   private:
    void launch_wrapper() {
        [](dstd::SharedPtr<WhenAllVectorState<Awaitable>> s) -> FireAndForget {
            for (;;) {
                const std::size_t index = s->claims.claim();
                if (index >= s->total) co_return;
                std::exception_ptr ex;
                try {
                    s->results[index] =
                        co_await std::move(s->awaitables[index]);
                } catch (...) {
                    ex = std::current_exception();
                }
                if (ex) {
                    s->on_exception(std::move(ex));
                } else {
                    s->on_one_complete();
                }
            }
        }(state_);
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
 * dstd::Vector<SpawnFuture<int>> futures;
 * for (int i = 0; i < 100; i++) {
 *     futures.push_back(scope.spawn_future([]() -> CoroTask<int> {
 *         co_return compute();
 *     }));
 * }
 * auto results = co_await when_all(futures);
 * @endcode
 */
template <typename Awaitable>
auto when_all(dstd::Vector<Awaitable> awaitables) {
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
        dstd::Vector<Awaitable>(awaitables.begin(), awaitables.end()));
}

// ============================================================================
// Specialization for void result types
// ============================================================================

template <typename Awaitable>
    requires(std::is_void_v<typename Awaitable::result_type>)
struct WhenAllVectorState<Awaitable> : detail::WhenAllCompletionState {
    dstd::Vector<Awaitable> awaitables;
    detail::ChildClaims claims;

    explicit WhenAllVectorState(dstd::Vector<Awaitable> items)
        : awaitables(std::move(items)) {
        total = awaitables.size();
    }
};

template <typename Awaitable>
    requires(std::is_void_v<typename Awaitable::result_type>)
class WhenAllVectorAwaitable<Awaitable> {
   private:
    dstd::SharedPtr<WhenAllVectorState<Awaitable>> state_;

   public:
    using result_type = void;

    explicit WhenAllVectorAwaitable(dstd::Vector<Awaitable> awaitables)
        : state_(dstd::make_shared<WhenAllVectorState<Awaitable>>(
              std::move(awaitables))) {}

    /// Always return false: wrapper coroutines are launched in
    /// await_suspend, so we must always enter it.  If all sub-awaitables
    /// complete synchronously, await_suspend returns false (no suspend).
    bool await_ready() { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor = root->get_executor();
        }

        if (state_->total == 0) {
            return false;
        }

        while (!state_->claims.exhausted(state_->total)) launch_wrapper();

        // Check if all tasks completed synchronously during launch_wrapper
        if (state_->completed_count.load(std::memory_order_acquire) ==
            state_->total) {
            return false;  // Don't suspend
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async = true;
        }

        // We will suspend - mark it and double-check for completion.
        // Do this last: completion may schedule/resume and destroy this frame.
        state_->mark_suspended_and_check_completion();

        return true;
    }

    void await_resume() {
        state_->finish_wait();
        if (state_->exception) {
            common::rethrow_and_clear(state_->exception);
        }
    }

   private:
    void launch_wrapper() {
        [](dstd::SharedPtr<WhenAllVectorState<Awaitable>> s) -> FireAndForget {
            for (;;) {
                const std::size_t index = s->claims.claim();
                if (index >= s->total) co_return;
                std::exception_ptr ex;
                try {
                    co_await std::move(s->awaitables[index]);
                } catch (...) {
                    ex = std::current_exception();
                }
                if (ex) {
                    s->on_exception(std::move(ex));
                } else {
                    s->on_one_complete();
                }
            }
        }(state_);
    }
};

// ============================================================================
// when_all_result_t - maps void result to dstd::Monostate for tuple storage
// ============================================================================

template <typename T>
using when_all_result_t =
    std::conditional_t<std::is_void_v<T>, dstd::Monostate, T>;

// ============================================================================
// WhenAllTupleAwaitable - Heterogeneous awaitable types (variadic/tuple)
// Heap-allocated state pattern, mirrors WhenAllVectorState/Awaitable exactly.
// ============================================================================

/**
 * Shared state for @ref WhenAllTupleAwaitable.
 * Heap-allocated so lifetime extends beyond await_suspend.
 */
template <typename... Awaitables>
struct WhenAllTupleState : detail::WhenAllCompletionState {
    dstd::Tuple<Awaitables...> awaitables;
    dstd::Tuple<
        dstd::Optional<when_all_result_t<typename Awaitables::result_type>>...>
        results;

    explicit WhenAllTupleState(Awaitables&&... items)
        : awaitables(std::forward<Awaitables>(items)...) {
        total = sizeof...(Awaitables);
    }
};

/**
 * WhenAllTupleAwaitable - Lightweight awaitable wrapping heap-allocated state.
 *
 * Accepts heterogeneous awaitable types; returns dstd::Tuple of their results.
 * Void result types are mapped to dstd::Monostate in the tuple.
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
        dstd::Tuple<when_all_result_t<typename Awaitables::result_type>...>;

    explicit WhenAllTupleAwaitable(Awaitables&&... awaitables)
        : state_(dstd::make_shared<WhenAllTupleState<Awaitables...>>(
              std::forward<Awaitables>(awaitables)...)) {}

    /// Always return false: wrapper coroutines are launched in
    /// await_suspend, so we must always enter it.  If all sub-awaitables
    /// complete synchronously, await_suspend returns false (no suspend).
    bool await_ready() { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor = root->get_executor();
        }

        if constexpr (sizeof...(Awaitables) == 0) {
            return false;
        }

        launch_all(std::index_sequence_for<Awaitables...>{});

        // Check if all completed synchronously during launch
        if (state_->completed_count.load(std::memory_order_acquire) ==
            state_->total) {
            return false;  // Don't suspend
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async = true;
        }

        // We will suspend - mark it and double-check for completion.
        // Do this last: completion may schedule/resume and destroy this frame.
        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        state_->finish_wait();
        if (state_->exception) {
            common::rethrow_and_clear(state_->exception);
        }
        return build_result(std::index_sequence_for<Awaitables...>{});
    }

   private:
    dstd::SharedPtr<WhenAllTupleState<Awaitables...>> state_;

    template <std::size_t... Is>
    result_type build_result(std::index_sequence<Is...>) {
        return result_type{std::move(*std::get<Is>(state_->results))...};
    }

    template <std::size_t I>
    void launch_one() {
        using A = std::tuple_element_t<I, dstd::Tuple<Awaitables...>>;
        using R = typename A::result_type;

        [](dstd::SharedPtr<WhenAllTupleState<Awaitables...>> s)
            -> FireAndForget {
            std::exception_ptr ex;
            try {
                if constexpr (std::is_void_v<R>) {
                    co_await std::move(std::get<I>(s->awaitables));
                    std::get<I>(s->results).emplace(dstd::Monostate{});
                } else {
                    std::get<I>(s->results)
                        .emplace(
                            co_await std::move(std::get<I>(s->awaitables)));
                }
                s->on_one_complete();
            } catch (...) {
                ex = std::current_exception();
            }
            if (ex) s->on_exception(std::move(ex));
        }(state_);
    }

    /// Expand index sequence to launch all wrapper coroutines.
    template <std::size_t... Is>
    void launch_all(std::index_sequence<Is...>) {
        (launch_one<Is>(), ...);
    }
};

/**
 * when_all - Wait for all awaitables to complete (variadic/tuple version)
 *
 * Accepts 2+ heterogeneous awaitable types and returns a dstd::Tuple of
 * their results. Void result types appear as dstd::Monostate in the tuple.
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

#endif  // DFTRACER_UTILS_CORO_WHEN_ALL_H
