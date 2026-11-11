// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_WHEN_ANY_H
#define DFTRACER_UTILS_CORO_WHEN_ANY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/detail/completion_state.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

// Timer service needed for TimeoutAwaitable
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::coro {

/// Maps void result types to dstd::Monostate for use in dstd::Variant.
template <typename T>
using when_any_result_t =
    std::conditional_t<std::is_void_v<T>, dstd::Monostate, T>;

namespace detail {

/// Runs a cancel() that may throw in a frame, which keeps what it threw in
/// `slot`, since coro code cannot catch.
struct CaptureCall {
    struct promise_type : FrameAllocation {
        std::exception_ptr* slot;

        template <typename A>
        promise_type(A&, std::exception_ptr& error) noexcept : slot(&error) {}

        CaptureCall get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            *slot = std::current_exception();
        }
    };
};

template <typename A>
CaptureCall cancel_capturing(A& awaitable,
                             [[maybe_unused]] std::exception_ptr& slot) {
    awaitable.cancel();
    co_return;
}

/// Calls `awaitable.cancel()`. One that may throw does not stop the others: the
/// first error goes to `first`, unless it already holds one.
template <typename A>
void cancel_loser(A& awaitable, std::exception_ptr& first,
                  std::atomic<bool>& failed) noexcept {
    if constexpr (noexcept(awaitable.cancel())) {
        awaitable.cancel();
    } else {
        std::exception_ptr error;
        cancel_capturing(awaitable, error);
        if (error && !first) {
            first = std::move(error);
            failed.store(true, std::memory_order_release);
        }
    }
}

/// A never-started loser with a cancel hook is dropped once a winner exists.
template <typename A>
bool never_started(A& a) {
    if constexpr (requires(A& x) {
                      x.started();
                      x.cancel();
                  }) {
        return !a.started();
    } else {
        return false;
    }
}

}  // namespace detail

// ============================================================================
// WhenAnyResult - Result of when_any operation
// ============================================================================

/**
 * WhenAnyResult - Contains index and result of first-completed operation
 *
 * @tparam T Result type
 */
template <typename T>
struct WhenAnyResult {
    std::size_t index;  ///< Index of completed operation
    T result;           ///< Result from completed operation

    /**
     * Helper to request cancellation on all remaining futures
     * This is cooperative cancellation - a @ref Scope child ends cancelled at
     * its next wait
     */
    void cancel_remaining() const {
        for (auto& token : remaining_cancellation_tokens) {
            if (token) {
                token->store(true, std::memory_order_release);
            }
        }
    }

    /// Cancellation tokens for remaining tasks
    dstd::Vector<dstd::SharedPtr<std::atomic<bool>>>
        remaining_cancellation_tokens;
};

/// Forward declaration for @ref SharedState
template <typename Awaitable>
struct WhenAnySharedState;

// ============================================================================
// WhenAnyAwaitable - Completes when first input completes
// ============================================================================

template <typename Awaitable>
struct WhenAnySharedState : detail::WhenAnyCompletionState {
    std::atomic<bool> completed{false};
    WhenAnyResult<typename Awaitable::result_type> result;
    std::exception_ptr exception;
    dstd::Vector<dstd::SharedPtr<std::atomic<bool>>> cancellation_tokens;
    dstd::Vector<Awaitable> awaitables;
    detail::ChildClaims claims;

    explicit WhenAnySharedState(dstd::Vector<Awaitable> aws)
        : awaitables(std::move(aws)) {
        wrappers_total = awaitables.size();
        cancellation_tokens.reserve(awaitables.size());

        for (auto& awaitable : awaitables) {
            if constexpr (requires { awaitable.cancellation_token(); }) {
                cancellation_tokens.push_back(awaitable.cancellation_token());
            } else {
                cancellation_tokens.push_back(nullptr);
            }
        }
    }

    void cancel_losers(std::size_t winner) noexcept {
        for (std::size_t j = 0; j < awaitables.size(); ++j) {
            if (cancellation_tokens[j]) {
                cancellation_tokens[j]->store(true, std::memory_order_release);
            }
            if (j != winner) {
                if constexpr (requires { awaitables[j].cancel(); }) {
                    detail::cancel_loser(awaitables[j], exception, failed);
                }
            }
        }
    }

    /// Runs once, by the winner. `error` is null when nothing threw. `stopped`
    /// is true when the winner stopped, which stops the awaiter.
    void finish_winner(std::size_t winner, std::exception_ptr* error,
                       bool winner_stopped) noexcept {
        if (error != nullptr) {
            exception = std::move(*error);
            failed.store(true, std::memory_order_release);
        }
        if (winner_stopped) stopped.store(true, std::memory_order_release);
        cancel_losers(winner);
        on_first_complete();
    }
};

/**
 * WhenAnyAwaitable - Completes when first awaitable completes
 *
 * Losers are cancelled and awaited before when_any returns. An awaitable may
 * expose cancellation_token() (its token is set) and cancel() (called once
 * from the winner's thread, possibly while the loser runs, so it must be
 * thread-safe and must make the loser finish). An awaitable
 * with neither runs to completion and delays the return. A loser that stops is
 * the expected end of a cancel. When the winner stops, the awaiting task stops
 * too.
 *
 * Usage:
 * @code
 * auto first = co_await when_any(fetch(a), fetch(b));
 * use(first.index, first.result);
 * @endcode
 */
template <typename Awaitable>
class WhenAnyAwaitable {
   private:
    using SharedState = WhenAnySharedState<Awaitable>;
    dstd::SharedPtr<SharedState> state_;

   public:
    using result_type = WhenAnyResult<typename Awaitable::result_type>;

    explicit WhenAnyAwaitable(dstd::Vector<Awaitable> awaitables)
        : state_(dstd::make_shared<SharedState>(std::move(awaitables))) {}

    ~WhenAnyAwaitable() = default;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->bind_awaiter(h);

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor = root->get_executor();
        }

        if (state_->awaitables.empty()) {
            return false;
        }

        // Before the first child starts: a winner on another worker reads a
        // loser's executor in cancel(), so no child may bind one late.
        Executor* executor = state_->executor != nullptr
                                 ? state_->executor
                                 : detail::current_executor();
        for (auto& awaitable : state_->awaitables) {
            if constexpr (requires { awaitable.handle().promise().executor; }) {
                auto& promise = awaitable.handle().promise();
                if (promise.executor == nullptr) promise.executor = executor;
            }
        }

        while (!state_->claims.exhausted(state_->wrappers_total)) {
            launch_wrapper();
        }

        if (state_->released()) {
            return state_->end_stopped();
        }

        // Set awaiting_async BEFORE mark_suspended_and_check_completion.
        // After that call, another thread may complete a task and destroy
        // this frame, so h.promise() must not be accessed afterward.
        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async = true;
        }

        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        state_->finish_wait();
        state_->awaitables.clear();
        if (state_->exception) {
            common::rethrow_and_clear(state_->exception);
        }
        return std::move(state_->result);
    }

   private:
    void launch_wrapper() {
        // Fire-and-forget wrapper. It takes children until none is left or one
        // suspends it, then the launcher starts another wrapper.
        auto wrapper =
            [](dstd::SharedPtr<SharedState> state) -> coro::detail::Coro {
            for (;;) {
                const std::size_t index = state->claims.claim();
                // Not awaitables.size(): the awaiter clears the vector after
                // the last child reports in.
                if (index >= state->wrappers_total) co_return;
                if (state->completed.load(std::memory_order_acquire) &&
                    detail::never_started(state->awaitables[index])) {
                    state->on_wrapper_finished();
                    continue;
                }
                auto outcome =
                    co_await settle_in_place(state->awaitables[index]);
                bool expected = false;
                const bool won = state->completed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel);
                if (won) {
                    state->result.index = index;
                    if (!outcome.exception && !outcome.stopped) {
                        if constexpr (!std::is_void_v<
                                          typename Awaitable::result_type>) {
                            state->result.result = std::move(*outcome.value);
                        }
                    }
                    state->result.remaining_cancellation_tokens.reserve(
                        state->cancellation_tokens.size() - 1);
                    for (std::size_t j = 0;
                         j < state->cancellation_tokens.size(); ++j) {
                        if (j != index && state->cancellation_tokens[j]) {
                            state->result.remaining_cancellation_tokens
                                .push_back(state->cancellation_tokens[j]);
                        }
                    }
                    std::exception_ptr error = std::move(outcome.exception);
                    state->finish_winner(index, error ? &error : nullptr,
                                         outcome.stopped);
                }
                state->on_wrapper_finished();
            }
        }(state_);

        // Set executor on the Coro's promise so FinalAwaiter can
        // schedule deferred destruction via the worker's TLS list.
        wrapper.handle().promise().executor = state_->executor;
        // Release ownership: the executor manages the frame from
        // now on.  FinalAwaiter will see released==true and schedule
        // deferred destruction when the wrapper completes.
        auto h = wrapper.release();
        // Resume past initial_suspend.  The wrapper will either:
        h.resume();
    }
};

/**
 * when_any - Race multiple operations, return first to complete
 *
 * @param awaitables Vector of awaitables to race
 * @return @ref WhenAnyResult with index and result of first completion
 *
 * Usage:
 * @code
 * dstd::Vector<coro::CoroTask<int>> reads;
 * reads.push_back(read_cache());
 * reads.push_back(read_disk());
 * auto result = co_await when_any(std::move(reads));
 *
 * switch (result.index) {
 *     case 0: std::cout << "Cache hit!\n"; break;
 *     case 1: std::cout << "Local disk\n"; break;
 * }
 * process(result.result);
 * @endcode
 */
template <typename Awaitable>
auto when_any(dstd::Vector<Awaitable> awaitables) {
    return WhenAnyAwaitable<Awaitable>(std::move(awaitables));
}

/**
 * when_any - Race multiple operations (initializer_list version)
 *
 * Usage:
 * @code
 * auto result = co_await when_any({future1, future2, future3});
 * @endcode
 */
template <typename Awaitable>
auto when_any(std::initializer_list<Awaitable> awaitables) {
    return WhenAnyAwaitable<Awaitable>(
        dstd::Vector<Awaitable>(awaitables.begin(), awaitables.end()));
}

// ============================================================================
// Helper: when_any with variadic arguments
// ============================================================================

namespace detail {

/// Helper to convert variadic awaitables to vector
/// All awaitables must be the same type
template <typename Awaitable, typename... Rest>
dstd::Vector<Awaitable> make_awaitable_vector(Awaitable&& first,
                                              Rest&&... rest) {
    dstd::Vector<Awaitable> vec;
    vec.reserve(1 + sizeof...(Rest));
    vec.push_back(std::forward<Awaitable>(first));
    (vec.push_back(std::forward<Rest>(rest)), ...);
    return vec;
}

}  // namespace detail

/**
 * when_any - Race multiple operations (variadic version)
 *
 * All awaitables must be the same type.
 *
 * Usage:
 * @code
 * auto result = co_await when_any(future1, future2, future3);
 * @endcode
 */
template <typename Awaitable, typename... Rest,
          typename = std::enable_if_t<std::conjunction_v<
              std::is_same<std::decay_t<Awaitable>, std::decay_t<Rest>>...>>>
auto when_any(Awaitable&& first, Rest&&... rest) {
    return WhenAnyAwaitable<std::decay_t<Awaitable>>(
        detail::make_awaitable_vector(std::forward<Awaitable>(first),
                                      std::forward<Rest>(rest)...));
}

// ============================================================================
// Heterogeneous when_any - different awaitable types, variant result
// ============================================================================

/**
 * WhenAnyTupleResult - Result of heterogeneous when_any.
 *
 * @tparam Awaitables Pack of distinct awaitable types.
 */
template <typename... Awaitables>
struct WhenAnyTupleResult {
    std::size_t index;

    /// Type-safe index-based access to the winning result.
    /// N must equal index at runtime, otherwise std::bad_variant_access.
    /// Works correctly even when multiple awaitables share the same
    /// result type (e.g. variant<float, int, int, float>).
    template <std::size_t N>
    auto& get() & {
        return std::get<N>(result_);
    }

    template <std::size_t N>
    auto&& get() && {
        return std::get<N>(std::move(result_));
    }

    template <std::size_t N>
    const auto& get() const& {
        return std::get<N>(result_);
    }

    void cancel_remaining() const {
        for (auto& token : remaining_cancellation_tokens_) {
            if (token) {
                token->store(true, std::memory_order_release);
            }
        }
    }

   private:
    // Internal state - accessed by WhenAnyTupleState and
    // WhenAnyTupleAwaitable via friendship.
    template <typename... As>
    friend struct WhenAnyTupleState;

    template <typename... As>
    friend class WhenAnyTupleAwaitable;

    dstd::Variant<when_any_result_t<typename Awaitables::result_type>...>
        result_;
    dstd::Vector<dstd::SharedPtr<std::atomic<bool>>>
        remaining_cancellation_tokens_;
};

template <typename... Awaitables>
struct WhenAnyTupleState : detail::WhenAnyCompletionState {
    static constexpr std::size_t TOTAL = sizeof...(Awaitables);

    dstd::Optional<dstd::Tuple<Awaitables...>> awaitables;
    std::atomic<bool> completed{false};
    WhenAnyTupleResult<Awaitables...> result;
    std::exception_ptr exception;
    dstd::Vector<dstd::SharedPtr<std::atomic<bool>>> cancellation_tokens;

    explicit WhenAnyTupleState(Awaitables&&... aws)
        : awaitables(std::in_place, std::forward<Awaitables>(aws)...) {
        wrappers_total = TOTAL;
        cancellation_tokens.reserve(TOTAL);
        std::apply(
            [this](auto&... a) {
                (
                    [&](auto& aw) {
                        if constexpr (requires { aw.cancellation_token(); }) {
                            cancellation_tokens.push_back(
                                aw.cancellation_token());
                        } else {
                            cancellation_tokens.push_back(nullptr);
                        }
                    }(a),
                    ...);
            },
            *awaitables);
    }

    /// True for the first wrapper to call, which then owns the result.
    bool claim() {
        bool expected = false;
        return completed.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel);
    }

    template <std::size_t I, typename V>
    void store_winner(V&& value) {
        result.result_.template emplace<I>(std::forward<V>(value));
        record_winner<I>();
    }

    template <std::size_t I>
    void record_winner() {
        result.index = I;

        result.remaining_cancellation_tokens_.reserve(TOTAL - 1);
        for (std::size_t j = 0; j < cancellation_tokens.size(); ++j) {
            if (j != I && cancellation_tokens[j]) {
                result.remaining_cancellation_tokens_.push_back(
                    cancellation_tokens[j]);
            }
        }
    }

    /// Runs once, by the winner. `error` is null when nothing threw.
    /// `winner_stopped` stops the awaiter.
    template <std::size_t I>
    void finish_winner(std::exception_ptr* error,
                       bool winner_stopped) noexcept {
        if (error != nullptr) {
            exception = std::move(*error);
            failed.store(true, std::memory_order_release);
        }
        if (winner_stopped) stopped.store(true, std::memory_order_release);
        cancel_losers(I, std::make_index_sequence<TOTAL>{});
        on_first_complete();
    }

    template <std::size_t... Is>
    void cancel_losers(std::size_t winner,
                       std::index_sequence<Is...>) noexcept {
        (
            [&] {
                if (cancellation_tokens[Is]) {
                    cancellation_tokens[Is]->store(true,
                                                   std::memory_order_release);
                }
                if (Is != winner) {
                    if constexpr (requires {
                                      std::get<Is>(*awaitables).cancel();
                                  }) {
                        detail::cancel_loser(std::get<Is>(*awaitables),
                                             exception, failed);
                    }
                }
            }(),
            ...);
    }
};

template <typename... Awaitables>
class WhenAnyTupleAwaitable {
   private:
    using State = WhenAnyTupleState<Awaitables...>;
    dstd::SharedPtr<State> state_;

    /// Launch a fire-and-forget Coro wrapper for the I-th awaitable.
    template <std::size_t I>
    void launch_one() {
        using A = std::tuple_element_t<I, dstd::Tuple<Awaitables...>>;
        auto wrapper = [](dstd::SharedPtr<State> state) -> coro::detail::Coro {
            if (state->completed.load(std::memory_order_acquire) &&
                detail::never_started(std::get<I>(*state->awaitables))) {
                state->on_wrapper_finished();
                co_return;
            }
            auto outcome =
                co_await settle_in_place(std::get<I>(*state->awaitables));
            if (state->claim()) {
                if (!outcome.exception && !outcome.stopped) {
                    if constexpr (std::is_void_v<typename A::result_type>) {
                        state->template store_winner<I>(dstd::Monostate{});
                    } else {
                        state->template store_winner<I>(
                            std::move(*outcome.value));
                    }
                } else {
                    state->template record_winner<I>();
                }
                std::exception_ptr error = std::move(outcome.exception);
                state->template finish_winner<I>(error ? &error : nullptr,
                                                 outcome.stopped);
            }
            state->on_wrapper_finished();
            co_return;
        }(state_);

        wrapper.handle().promise().executor = state_->executor;
        auto h = wrapper.release();
        h.resume();
    }

    template <std::size_t... Is>
    void launch_all(std::index_sequence<Is...>) {
        (launch_one<Is>(), ...);
    }

   public:
    using result_type = WhenAnyTupleResult<Awaitables...>;

    explicit WhenAnyTupleAwaitable(Awaitables&&... aws)
        : state_(dstd::make_shared<State>(std::forward<Awaitables>(aws)...)) {}

    ~WhenAnyTupleAwaitable() = default;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->bind_awaiter(h);

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor = root->get_executor();
        }

        launch_all(std::make_index_sequence<sizeof...(Awaitables)>{});

        if (state_->released()) {
            return state_->end_stopped();
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async = true;
        }

        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        state_->finish_wait();
        state_->awaitables.reset();
        if (state_->exception) {
            common::rethrow_and_clear(state_->exception);
        }
        return std::move(state_->result);
    }
};

/**
 * when_any - Race heterogeneous awaitables, return first to complete.
 *
 * Selected only when not all types are the same (the homogeneous variadic
 * overload handles the identical-type case).
 *
 * @return @ref WhenAnyTupleResult with variant result and winner index.
 */
template <typename A1, typename A2, typename... Rest>
    requires(!std::conjunction_v<
             std::is_same<std::decay_t<A1>, std::decay_t<A2>>,
             std::is_same<std::decay_t<A1>, std::decay_t<Rest>>...>)
auto when_any(A1&& a1, A2&& a2, Rest&&... rest) {
    return WhenAnyTupleAwaitable<std::decay_t<A1>, std::decay_t<A2>,
                                 std::decay_t<Rest>...>(
        std::forward<A1>(a1), std::forward<A2>(a2),
        std::forward<Rest>(rest)...);
}

// ============================================================================
// Timeout support
// ============================================================================

/**
 * TimeoutAwaitable - Awaitable that completes after a duration
 *
 * Can be used with when_any to implement timeouts:
 *
 * @code
 * auto* timers = Executor::current()->timers();
 * auto result = co_await when_any(read_task(), timeout(5s, timers));
 * if (result.index == 1) {
 *     // Timeout occurred
 * }
 * @endcode
 *
 * co_await returns when the time passes. After cancel() or executor shutdown
 * the awaiting task stops. cancel() is thread-safe.
 */
template <typename Duration>
class TimeoutAwaitable {
   private:
    enum class Outcome : std::uint8_t { Pending, Fired, Cancelled };

    struct State {
        CompletionLatch latch;
        std::coroutine_handle<> handle{};
        Executor* executor = nullptr;
        TimerService* timers = nullptr;
        std::atomic<TimerService::TimerId> timer{0};
        Outcome outcome = Outcome::Pending;
        detail::ParkNode* park = nullptr;
        detail::ParkOwner owner;

        void cancel() noexcept {
            if (latch.on_cancelled()) {
                outcome = Outcome::Cancelled;
                cancel_timer();
                detail::stop_or_continue(executor, owner, handle);
            }
        }

        void on_fire() noexcept {
            if (latch.on_completed()) {
                outcome = Outcome::Fired;
                resume_continuation(executor, handle);
            }
        }

        void cancel_timer() noexcept {
            const TimerService::TimerId id =
                timer.exchange(0, std::memory_order_acq_rel);
            if (id != 0 && timers) timers->cancel_timeout(id);
        }

        static void cancel_thunk(void* self) noexcept {
            static_cast<State*>(self)->cancel();
        }
    };

    Duration duration_;
    TimerService* timer_service_;
    dstd::SharedPtr<State> state_;

   public:
    using result_type = void;

    explicit TimeoutAwaitable(Duration duration, TimerService* timer_service)
        : duration_(duration),
          timer_service_(timer_service),
          state_(dstd::make_shared<State>()) {}

    /// A copy waits on its own timer.
    TimeoutAwaitable(const TimeoutAwaitable& other)
        : duration_(other.duration_),
          timer_service_(other.timer_service_),
          state_(dstd::make_shared<State>()) {}
    TimeoutAwaitable(TimeoutAwaitable&&) noexcept = default;
    TimeoutAwaitable& operator=(const TimeoutAwaitable&) = delete;
    TimeoutAwaitable& operator=(TimeoutAwaitable&&) = delete;

    ~TimeoutAwaitable() {
        if (!state_) return;
        // A fire that comes after this finds the latch cancelled and resumes
        // nothing.
        state_->latch.on_cancelled();
        state_->cancel_timer();
        if (state_->park) state_->executor->unpark(state_->park);
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        TimerService* timers =
            timer_service_ != nullptr ? timer_service_ : &process_timers();

        State& st = *state_;
        st.handle = h;
        st.owner = detail::current_park_owner();
        st.executor = detail::resume_executor_for(nullptr);
        st.timers = timers;
        if (st.executor) {
            st.park = st.executor->park(&st, &State::cancel_thunk);
        }
        st.timer.store(timers->register_timeout(
                           duration_, [state = state_] { state->on_fire(); }),
                       std::memory_order_release);
        // From here on the fire or a cancel may resume the frame.
        const std::uint8_t before = st.latch.on_suspended();
        if (before & CompletionLatch::BIT_COMPLETED) {
            st.outcome = Outcome::Fired;
            return false;
        }
        if (before & CompletionLatch::BIT_CANCELLED) {
            st.outcome = Outcome::Cancelled;
            st.cancel_timer();
            return detail::stop_task(st.executor, nullptr, st.owner);
        }
        return true;
    }

    void await_resume() {
        State& st = *state_;
        if (st.park) {
            st.executor->unpark(st.park);
            st.park = nullptr;
        }
        st.cancel_timer();
        if (st.outcome == Outcome::Cancelled) {
            detail::cancelled_unstoppable("a timer wait");
        }
    }

    /// Ends the wait early. Thread-safe. Does nothing once the wait is over.
    void cancel() noexcept { state_->cancel(); }
};

/**
 * timeout - Create a timeout awaitable using @ref TimerService
 *
 * Usage:
 * @code
 * using namespace std::chrono_literals;
 * auto* timers = Executor::current()->timers();
 * auto result = co_await when_any(some_operation(), timeout(5s, timers));
 * @endcode
 *
 * A null @p timer_service uses @ref process_timers, the process-wide one.
 */
template <typename Rep, typename Period>
TimeoutAwaitable<std::chrono::duration<Rep, Period>> timeout(
    std::chrono::duration<Rep, Period> duration, TimerService* timer_service) {
    return TimeoutAwaitable<std::chrono::duration<Rep, Period>>(duration,
                                                                timer_service);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_WHEN_ANY_H
