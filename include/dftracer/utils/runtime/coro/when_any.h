#ifndef DFTRACER_UTILS_RUNTIME_CORO_WHEN_ANY_H
#define DFTRACER_UTILS_RUNTIME_CORO_WHEN_ANY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/completion_state.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/executor/resumption_helper.h>

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
#include <vector>

// Timer service needed for TimeoutAwaitable
#include <dftracer/utils/runtime/executor/timer_service.h>

namespace dftracer::utils::coro {

/// Maps void result types to std::monostate for use in std::variant.
template <typename T>
using when_any_result_t =
    std::conditional_t<std::is_void_v<T>, std::monostate, T>;

namespace detail {

/// Awaits `a` in place. An awaitable that ends as cancelled (it has
/// cancelled() and it is true) gives an empty result, not an exception, so a
/// loser that was stopped costs no throw.
template <typename A>
struct CancelAware {
    using Value = when_any_result_t<typename A::result_type>;

    A& inner;

    bool await_ready() { return inner.await_ready(); }

    template <typename Handle>
    auto await_suspend(Handle h) {
        return inner.await_suspend(h);
    }

    std::optional<Value> await_resume() {
        if constexpr (requires(const A& a) { a.cancelled(); }) {
            if (inner.cancelled()) return std::nullopt;
        }
        if constexpr (std::is_void_v<typename A::result_type>) {
            inner.await_resume();
            return Value{};
        } else {
            return Value(inner.await_resume());
        }
    }
};

/// A loser that has a cancel hook and was never started is dropped without
/// running once a winner exists.
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
     * This is cooperative cancellation - tasks must check
     * ctx.is_cancellation_requested()
     */
    void cancel_remaining() const {
        for (auto& token : remaining_cancellation_tokens) {
            if (token) {
                token->store(true, std::memory_order_release);
            }
        }
    }

    /// Cancellation tokens for remaining tasks
    std::vector<std::shared_ptr<std::atomic<bool>>>
        remaining_cancellation_tokens;
};

/// Forward declaration for SharedState
template <typename Awaitable>
struct WhenAnySharedState;

// ============================================================================
// WhenAnyAwaitable - Completes when first input completes
// ============================================================================

template <typename Awaitable>
struct WhenAnySharedState : WhenAnyCompletionState {
    std::atomic<bool> completed{false};
    WhenAnyResult<typename Awaitable::result_type> result;
    std::exception_ptr exception;
    std::vector<std::shared_ptr<std::atomic<bool>>> cancellation_tokens;
    /// The first awaitable to end was cancelled, so there is no result.
    bool cancelled_winner = false;
    std::vector<Awaitable> awaitables;
    ChildClaims claims;

    explicit WhenAnySharedState(std::vector<Awaitable> aws)
        : awaitables(std::move(aws)) {
        wrappers_total_ = awaitables.size();
        cancellation_tokens.reserve(awaitables.size());

        for (auto& awaitable : awaitables) {
            if constexpr (requires { awaitable.get_cancellation_token(); }) {
                cancellation_tokens.push_back(
                    awaitable.get_cancellation_token());
            } else {
                cancellation_tokens.push_back(nullptr);
            }
        }
    }

    /// Sets every token and calls cancel() on each loser that has one. A
    /// cancel() that throws does not stop the others; the first error is
    /// kept unless the winner already has one.
    void cancel_losers(std::size_t winner) noexcept {
        for (std::size_t j = 0; j < awaitables.size(); ++j) {
            if (cancellation_tokens[j]) {
                cancellation_tokens[j]->store(true, std::memory_order_release);
            }
            if (j != winner) {
                if constexpr (requires { awaitables[j].cancel(); }) {
                    try {
                        awaitables[j].cancel();
                    } catch (...) {
                        if (!exception) exception = std::current_exception();
                    }
                }
            }
        }
    }

    /// Releases the awaiter once the winner is recorded, with `*error` if the
    /// winner or its bookkeeping threw (null for none). Runs exactly once, by
    /// the winner.
    void finish_winner(std::size_t winner, std::exception_ptr* error) noexcept {
        if (error != nullptr) exception = std::move(*error);
        cancel_losers(winner);
        on_first_complete();
    }
};

/**
 * WhenAnyAwaitable - Completes when first awaitable completes
 *
 * Losers are cancelled and awaited before when_any returns, so no loser runs
 * after the awaiting coroutine resumes. Cancellation is cooperative: an
 * awaitable may expose get_cancellation_token() (its token is set) and
 * cancel() (called once from the winner's thread, possibly while the loser is
 * suspended or running, so it must be thread-safe and make the loser finish).
 * An awaitable with neither runs to completion and delays the return.
 *
 * Usage:
 * @code
 * auto result = co_await when_any({
 *     io::async_read(fd1, buf1, len1),
 *     io::async_read(fd2, buf2, len2),
 *     timeout(5s)
 * });
 *
 * if (result.index == 2) {
 *     // Timeout occurred
 * } else {
 *     // Got data from index 0 or 1
 *     process(result.result);
 * }
 * @endcode
 */
template <typename Awaitable>
class WhenAnyAwaitable {
   private:
    using SharedState = WhenAnySharedState<Awaitable>;
    std::shared_ptr<SharedState> state_;

   public:
    using result_type = WhenAnyResult<typename Awaitable::result_type>;

    explicit WhenAnyAwaitable(std::vector<Awaitable> awaitables)
        : state_(std::make_shared<SharedState>(std::move(awaitables))) {}

    ~WhenAnyAwaitable() = default;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine_ = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor_ = root->get_executor();
        }

        if (state_->awaitables.empty()) {
            return false;
        }

        while (!state_->claims.exhausted(state_->wrappers_total_)) {
            launch_wrapper();
        }

        if (state_->released()) {
            return false;
        }

        // Set awaiting_async BEFORE mark_suspended_and_check_completion.
        // After that call, another thread may complete a task and destroy
        // this frame, so h.promise() must not be accessed afterward.
        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async_ = true;
        }

        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        state_->finish_wait();
        state_->awaitables.clear();
        if (state_->exception) {
            rethrow_and_clear(state_->exception);
        }
        if (state_->cancelled_winner) throw_cancelled("wait cancelled");
        return std::move(state_->result);
    }

   private:
    void launch_wrapper() {
        // Fire-and-forget Coro wrapper. It takes the next child it can, awaits
        // it to the end and reports in on_wrapper_finished() for it, until no
        // child is left or one suspends it; the launcher then starts another
        // wrapper. on_wrapper_finished() resumes the awaiter after the last
        // child.
        auto wrapper = [](std::shared_ptr<SharedState> state) -> coro::Coro {
            for (;;) {
                const std::size_t index = state->claims.claim();
                // Not awaitables.size(): the awaiter clears the vector once the
                // last child has reported in, while this loop is still ending.
                if (index >= state->wrappers_total_) co_return;
                if (state->completed.load(std::memory_order_acquire) &&
                    detail::never_started(state->awaitables[index])) {
                    state->on_wrapper_finished();
                    continue;
                }
                bool won = false;
                try {
                    detail::CancelAware<Awaitable> aware{
                        state->awaitables[index]};
                    auto outcome = co_await aware;

                    bool expected = false;
                    won = state->completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel);
                    if (won) {
                        state->result.index = index;
                        if (outcome) {
                            state->result.result = std::move(*outcome);
                        } else {
                            state->cancelled_winner = true;
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
                        state->finish_winner(index, nullptr);
                    }
                } catch (...) {
                    bool expected = false;
                    if (won || state->completed.compare_exchange_strong(
                                   expected, true, std::memory_order_acq_rel)) {
                        std::exception_ptr error = std::current_exception();
                        state->finish_winner(index, &error);
                    }
                }
                state->on_wrapper_finished();
            }
        }(state_);

        // Set executor on the Coro's promise so FinalAwaiter can
        // schedule deferred destruction via the worker's TLS list.
        wrapper.handle().promise().executor = state_->executor_;
        // Release ownership: the executor manages the frame from
        // now on.  FinalAwaiter will see released==true and schedule
        // deferred destruction when the wrapper completes.
        auto h = wrapper.release();
        // Resume past initial_suspend.  The wrapper will either:
        // (a) suspend at co_await on the awaitable (most common), or
        // (b) run to completion if the awaitable was already ready.
        h.resume();
    }
};

/**
 * when_any - Race multiple operations, return first to complete
 *
 * @param awaitables Vector of awaitables to race
 * @return WhenAnyResult with index and result of first completion
 *
 * Usage:
 * @code
 * auto result = co_await when_any({
 *     io::async_read(cache_fd, buf, len),
 *     io::async_read(disk_fd, buf, len),
 *     io::async_read(net_fd, buf, len)
 * });
 *
 * switch (result.index) {
 *     case 0: std::cout << "Cache hit!\n"; break;
 *     case 1: std::cout << "Local disk\n"; break;
 *     case 2: std::cout << "Network fetch\n"; break;
 * }
 * process(result.result);
 * @endcode
 */
template <typename Awaitable>
auto when_any(std::vector<Awaitable> awaitables) {
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
        std::vector<Awaitable>(awaitables.begin(), awaitables.end()));
}

// ============================================================================
// Helper: when_any with variadic arguments
// ============================================================================

namespace detail {

/// Helper to convert variadic awaitables to vector
/// All awaitables must be the same type
template <typename Awaitable, typename... Rest>
std::vector<Awaitable> make_awaitable_vector(Awaitable&& first,
                                             Rest&&... rest) {
    std::vector<Awaitable> vec;
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

    std::variant<when_any_result_t<typename Awaitables::result_type>...>
        result_;
    std::vector<std::shared_ptr<std::atomic<bool>>>
        remaining_cancellation_tokens_;
};

template <typename... Awaitables>
struct WhenAnyTupleState : WhenAnyCompletionState {
    static constexpr std::size_t total_ = sizeof...(Awaitables);

    std::optional<std::tuple<Awaitables...>> awaitables_;
    std::atomic<bool> completed{false};
    WhenAnyTupleResult<Awaitables...> result;
    std::exception_ptr exception;
    /// The first awaitable to end was cancelled, so there is no result.
    bool cancelled_winner = false;
    std::vector<std::shared_ptr<std::atomic<bool>>> cancellation_tokens;

    explicit WhenAnyTupleState(Awaitables&&... aws)
        : awaitables_(std::in_place, std::forward<Awaitables>(aws)...) {
        wrappers_total_ = total_;
        cancellation_tokens.reserve(total_);
        std::apply(
            [this](auto&... a) {
                (
                    [&](auto& aw) {
                        if constexpr (requires {
                                          aw.get_cancellation_token();
                                      }) {
                            cancellation_tokens.push_back(
                                aw.get_cancellation_token());
                        } else {
                            cancellation_tokens.push_back(nullptr);
                        }
                    }(a),
                    ...);
            },
            *awaitables_);
    }

    /// True for the first wrapper to call; it then owns the result.
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

    /// Records the index and the tokens of the losers.
    template <std::size_t I>
    void record_winner() {
        result.index = I;

        result.remaining_cancellation_tokens_.reserve(total_ - 1);
        for (std::size_t j = 0; j < cancellation_tokens.size(); ++j) {
            if (j != I && cancellation_tokens[j]) {
                result.remaining_cancellation_tokens_.push_back(
                    cancellation_tokens[j]);
            }
        }
    }

    /// Releases the awaiter once the winner is recorded, with `error` if the
    /// winner or its bookkeeping threw. Runs exactly once, by the winner.
    template <std::size_t I>
    void finish_winner(std::exception_ptr* error) noexcept {
        if (error != nullptr) exception = std::move(*error);
        cancel_losers(I, std::make_index_sequence<total_>{});
        on_first_complete();
    }

    /// Sets every token and calls cancel() on each loser that has one. A
    /// cancel() that throws does not stop the others; the first error is
    /// kept unless the winner already has one.
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
                                      std::get<Is>(*awaitables_).cancel();
                                  }) {
                        try {
                            std::get<Is>(*awaitables_).cancel();
                        } catch (...) {
                            if (!exception)
                                exception = std::current_exception();
                        }
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
    std::shared_ptr<State> state_;

    /// Launch a fire-and-forget Coro wrapper for the I-th awaitable.
    template <std::size_t I>
    void launch_one() {
        using A = std::tuple_element_t<I, std::tuple<Awaitables...>>;
        auto wrapper = [](std::shared_ptr<State> state) -> coro::Coro {
            if (state->completed.load(std::memory_order_acquire) &&
                detail::never_started(std::get<I>(*state->awaitables_))) {
                state->on_wrapper_finished();
                co_return;
            }
            bool won = false;
            try {
                detail::CancelAware<A> aware{std::get<I>(*state->awaitables_)};
                auto outcome = co_await aware;
                won = state->claim();
                if (won) {
                    if (outcome) {
                        state->template store_winner<I>(std::move(*outcome));
                    } else {
                        state->template record_winner<I>();
                        state->cancelled_winner = true;
                    }
                    state->template finish_winner<I>(nullptr);
                }
            } catch (...) {
                if (won || state->claim()) {
                    std::exception_ptr error = std::current_exception();
                    state->template finish_winner<I>(&error);
                }
            }
            state->on_wrapper_finished();
            co_return;
        }(state_);

        wrapper.handle().promise().executor = state_->executor_;
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
        : state_(std::make_shared<State>(std::forward<Awaitables>(aws)...)) {}

    ~WhenAnyTupleAwaitable() = default;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine_ = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor_ = root->get_executor();
        }

        launch_all(std::make_index_sequence<sizeof...(Awaitables)>{});

        if (state_->released()) {
            return false;
        }

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async_ = true;
        }

        state_->mark_suspended_and_check_completion();

        return true;
    }

    result_type await_resume() {
        state_->finish_wait();
        state_->awaitables_.reset();
        if (state_->exception) {
            rethrow_and_clear(state_->exception);
        }
        if (state_->cancelled_winner) throw_cancelled("wait cancelled");
        return std::move(state_->result);
    }
};

/**
 * when_any - Race heterogeneous awaitables, return first to complete.
 *
 * Selected only when not all types are the same (the homogeneous variadic
 * overload handles the identical-type case).
 *
 * @return WhenAnyTupleResult with variant result and winner index.
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
 * auto result = co_await when_any({
 *     io::async_read(fd, buf, len),
 *     timeout(std::chrono::seconds(5))
 * });
 *
 * if (result.index == 1) {
 *     // Timeout occurred
 * }
 * @endcode
 *
 * co_await throws "Operation timed out" when the time passes, and a cancelled
 * error (see coro::is_cancelled) after cancel() or when the executor shuts
 * down. cancel() is thread-safe and may be called at any time, so a timeout
 * that loses a when_any race does not hold the winner back.
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
        ParkNode* park = nullptr;

        void cancel() noexcept {
            if (latch.on_cancelled()) {
                outcome = Outcome::Cancelled;
                cancel_timer();
                resume_continuation(executor, handle);
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
    std::shared_ptr<State> state_;

   public:
    using result_type = void;

    explicit TimeoutAwaitable(Duration duration, TimerService* timer_service)
        : duration_(duration),
          timer_service_(timer_service),
          state_(std::make_shared<State>()) {}

    /// A copy waits on its own timer.
    TimeoutAwaitable(const TimeoutAwaitable& other)
        : duration_(other.duration_),
          timer_service_(other.timer_service_),
          state_(std::make_shared<State>()) {}
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
        if (!timer_service_) {
            throw DFTUtilsException(
                ErrorCode::Pipeline,
                "TimeoutAwaitable: TimerService not available");
        }

        State& st = *state_;
        st.handle = h;
        st.executor = resume_executor_for(nullptr);
        st.timers = timer_service_;
        if (st.executor) {
            st.park = st.executor->park(&st, &State::cancel_thunk);
        }
        st.timer.store(timer_service_->register_timeout(
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
            return false;
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
            throw_cancelled("timeout cancelled");
        }
        throw DFTUtilsException(ErrorCode::Pipeline, "Operation timed out");
    }

    /// Ends the wait early. Thread-safe; does nothing once the wait is over.
    void cancel() noexcept { state_->cancel(); }
};

/**
 * timeout - Create a timeout awaitable using TimerService
 *
 * Usage:
 * @code
 * using namespace std::chrono_literals;
 * auto& timer_service = ctx.get_executor()->get_timer_service();
 * auto result = co_await when_any({
 *     some_operation(),
 *     timeout(5s, &timer_service)
 * });
 * @endcode
 */
template <typename Rep, typename Period>
TimeoutAwaitable<std::chrono::duration<Rep, Period>> timeout(
    std::chrono::duration<Rep, Period> duration, TimerService* timer_service) {
    return TimeoutAwaitable<std::chrono::duration<Rep, Period>>(duration,
                                                                timer_service);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_WHEN_ANY_H
