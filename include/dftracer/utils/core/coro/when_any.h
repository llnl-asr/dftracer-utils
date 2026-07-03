#ifndef DFTRACER_UTILS_CORE_CORO_WHEN_ANY_H
#define DFTRACER_UTILS_CORE_CORO_WHEN_ANY_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/exception_helpers.h>
#include <dftracer/utils/core/coro/completion_state.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/resumption_helper.h>
#include <dftracer/utils/core/coro/task.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
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
#include <dftracer/utils/core/common/timer_service.h>

namespace dftracer::utils::coro {

// Maps void result types to std::monostate for use in std::variant.
template <typename T>
using when_any_result_t =
    std::conditional_t<std::is_void_v<T>, std::monostate, T>;

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

    // Cancellation tokens for remaining tasks
    std::vector<std::shared_ptr<std::atomic<bool>>>
        remaining_cancellation_tokens;
};

// Forward declaration for SharedState
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
    // Wrappers are fire-and-forget Coro instances managed by the
    // executor's FinalAwaiter lifecycle.  No wrapper_tasks vector is
    // needed -- the executor owns the frames after release().
    std::atomic<std::size_t> wrappers_done{0};
    std::size_t total_wrappers{0};
    std::vector<Awaitable> awaitables;

    explicit WhenAnySharedState(std::vector<Awaitable> aws)
        : awaitables(std::move(aws)) {
        total_wrappers = awaitables.size();
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

    ~WhenAnySharedState() {
        // Detach all awaitables to prevent their completion paths from
        // resuming wrapper coroutine handles.  Wrappers are managed by
        // the executor (released Coro instances) so we do not destroy
        // them here -- detach() schedules stale handles for deferred
        // destruction via the executor's destroy_queue.
        for (auto& a : awaitables) {
            if constexpr (requires { a.detach(); }) {
                a.detach();
            }
        }
    }
};

/**
 * WhenAnyAwaitable - Completes when first awaitable completes
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

    bool await_ready() {
        for (std::size_t i = 0; i < state_->awaitables.size(); i++) {
            if (state_->awaitables[i].await_ready()) {
                try {
                    state_->result.index = i;
                    state_->result.result =
                        state_->awaitables[i].await_resume();

                    state_->result.remaining_cancellation_tokens.reserve(
                        state_->cancellation_tokens.size() - 1);
                    for (std::size_t j = 0;
                         j < state_->cancellation_tokens.size(); ++j) {
                        if (j != i && state_->cancellation_tokens[j]) {
                            state_->result.remaining_cancellation_tokens
                                .push_back(state_->cancellation_tokens[j]);
                        }
                    }

                    state_->completed.store(true, std::memory_order_release);
                    return true;
                } catch (...) {
                    state_->exception = std::current_exception();
                    state_->completed.store(true, std::memory_order_release);
                    return true;
                }
            }
        }
        return false;
    }

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

        for (std::size_t i = 0; i < state_->awaitables.size(); i++) {
            launch_wrapper(i);
        }

        if (state_->completed.load(std::memory_order_acquire)) {
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
        // Detach awaitables so that completing spawned tasks don't
        // try to resume wrapper Coro handles that may already have
        // been destroyed by the executor.  detach() now schedules
        // any stale waiter handles for deferred destruction, which
        // prevents the race between SpawnFuture::complete() and
        // frame cleanup.
        for (auto& token : state_->cancellation_tokens) {
            if (token) token->store(true, std::memory_order_release);
        }
        for (auto& a : state_->awaitables) {
            if constexpr (requires { a.detach(); }) {
                a.detach();
            }
        }
        if (state_->exception) {
            rethrow_and_clear(state_->exception);
        }
        return std::move(state_->result);
    }

   private:
    void launch_wrapper(std::size_t i) {
        // Create a fire-and-forget Coro wrapper.  After release(),
        // the executor owns the frame and FinalAwaiter will schedule
        // deferred destruction when the wrapper completes.  This
        // eliminates the circular reference (SharedState no longer
        // owns wrapper frames) and the race where wrapper_tasks.clear()
        // could destroy a frame that was enqueued for resumption.
        auto wrapper = [](std::shared_ptr<SharedState> state,
                          std::size_t index) -> coro::Coro {
            try {
                if (state->completed.load(std::memory_order_acquire)) {
                    state->wrappers_done.fetch_add(1,
                                                   std::memory_order_release);
                    co_return;
                }

                auto result = co_await std::move(state->awaitables[index]);

                bool expected = false;
                if (state->completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    state->result.index = index;
                    state->result.result = std::move(result);

                    state->result.remaining_cancellation_tokens.reserve(
                        state->cancellation_tokens.size() - 1);
                    for (std::size_t j = 0;
                         j < state->cancellation_tokens.size(); ++j) {
                        if (j != index && state->cancellation_tokens[j]) {
                            state->result.remaining_cancellation_tokens
                                .push_back(state->cancellation_tokens[j]);
                        }
                    }

                    state->on_first_complete();
                }

                state->wrappers_done.fetch_add(1, std::memory_order_release);
            } catch (...) {
                auto ex = std::current_exception();
                if (state->completed.load(std::memory_order_acquire)) {
                    state->wrappers_done.fetch_add(1,
                                                   std::memory_order_release);
                    co_return;
                }

                bool expected = false;
                if (state->completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    state->exception = std::move(ex);
                    state->on_first_complete();
                }

                state->wrappers_done.fetch_add(1, std::memory_order_release);
            }
            co_return;
        }(state_, i);

        // Set executor on the Coro's promise so FinalAwaiter can
        // schedule deferred destruction via the worker's TLS list.
        wrapper.handle().promise().executor = state_->executor_;
        // Release ownership: the executor manages the frame from
        // now on.  FinalAwaiter will see released==true and schedule
        // deferred destruction when the wrapper completes.
        auto h = wrapper.release();
        // Resume past initial_suspend.  The wrapper will either:
        // (a) suspend at co_await SpawnFuture (most common), or
        // (b) run to completion if SpawnFuture was already ready.
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

// Helper to convert variadic awaitables to vector
// All awaitables must be the same type
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
// Heterogeneous when_any — different awaitable types, variant result
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
    // Internal state — accessed by WhenAnyTupleState and
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

    std::tuple<Awaitables...> awaitables_;
    std::atomic<bool> completed{false};
    WhenAnyTupleResult<Awaitables...> result;
    std::exception_ptr exception;
    std::vector<std::shared_ptr<std::atomic<bool>>> cancellation_tokens;

    explicit WhenAnyTupleState(Awaitables&&... aws)
        : awaitables_(std::forward<Awaitables>(aws)...) {
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
            awaitables_);
    }

    ~WhenAnyTupleState() {
        std::apply(
            [](auto&... a) {
                (
                    [&](auto& aw) {
                        if constexpr (requires { aw.detach(); }) {
                            aw.detach();
                        }
                    }(a),
                    ...);
            },
            awaitables_);
    }
};

template <typename... Awaitables>
class WhenAnyTupleAwaitable {
   private:
    using State = WhenAnyTupleState<Awaitables...>;
    std::shared_ptr<State> state_;

    // Launch a fire-and-forget Coro wrapper for the I-th awaitable.
    template <std::size_t I>
    void launch_one() {
        using A = std::tuple_element_t<I, std::tuple<Awaitables...>>;
        using R = typename A::result_type;

        auto wrapper = [](std::shared_ptr<State> state) -> coro::Coro {
            try {
                if (state->completed.load(std::memory_order_acquire)) {
                    co_return;
                }

                if constexpr (std::is_void_v<R>) {
                    co_await std::move(std::get<I>(state->awaitables_));

                    bool expected = false;
                    if (state->completed.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        state->result.index = I;
                        state->result.result_.template emplace<I>(
                            std::monostate{});

                        state->result.remaining_cancellation_tokens_.reserve(
                            State::total_ - 1);
                        for (std::size_t j = 0;
                             j < state->cancellation_tokens.size(); ++j) {
                            if (j != I && state->cancellation_tokens[j]) {
                                state->result.remaining_cancellation_tokens_
                                    .push_back(state->cancellation_tokens[j]);
                            }
                        }

                        state->on_first_complete();
                    }
                } else {
                    auto r =
                        co_await std::move(std::get<I>(state->awaitables_));

                    bool expected = false;
                    if (state->completed.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        state->result.index = I;
                        state->result.result_.template emplace<I>(std::move(r));

                        state->result.remaining_cancellation_tokens_.reserve(
                            State::total_ - 1);
                        for (std::size_t j = 0;
                             j < state->cancellation_tokens.size(); ++j) {
                            if (j != I && state->cancellation_tokens[j]) {
                                state->result.remaining_cancellation_tokens_
                                    .push_back(state->cancellation_tokens[j]);
                            }
                        }

                        state->on_first_complete();
                    }
                }
            } catch (...) {
                auto ex = std::current_exception();
                if (state->completed.load(std::memory_order_acquire)) {
                    co_return;
                }

                bool expected = false;
                if (state->completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    state->exception = std::move(ex);
                    state->on_first_complete();
                }
            }
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

    // Check if the I-th awaitable is immediately ready; if so, populate
    // state and return true.
    template <std::size_t I>
    bool check_ready_one() {
        using A = std::tuple_element_t<I, std::tuple<Awaitables...>>;
        using R = typename A::result_type;

        auto& aw = std::get<I>(state_->awaitables_);
        if (!aw.await_ready()) {
            return false;
        }

        try {
            state_->result.index = I;
            if constexpr (std::is_void_v<R>) {
                aw.await_resume();
                state_->result.result_.template emplace<I>(std::monostate{});
            } else {
                state_->result.result_.template emplace<I>(aw.await_resume());
            }

            state_->result.remaining_cancellation_tokens_.reserve(
                State::total_ - 1);
            for (std::size_t j = 0; j < state_->cancellation_tokens.size();
                 ++j) {
                if (j != I && state_->cancellation_tokens[j]) {
                    state_->result.remaining_cancellation_tokens_.push_back(
                        state_->cancellation_tokens[j]);
                }
            }

            state_->completed.store(true, std::memory_order_release);
        } catch (...) {
            state_->exception = std::current_exception();
            state_->completed.store(true, std::memory_order_release);
        }
        return true;
    }

    template <std::size_t... Is>
    bool check_ready_any(std::index_sequence<Is...>) {
        return (check_ready_one<Is>() || ...);
    }

   public:
    using result_type = WhenAnyTupleResult<Awaitables...>;

    explicit WhenAnyTupleAwaitable(Awaitables&&... aws)
        : state_(std::make_shared<State>(std::forward<Awaitables>(aws)...)) {}

    ~WhenAnyTupleAwaitable() = default;

    bool await_ready() {
        return check_ready_any(
            std::make_index_sequence<sizeof...(Awaitables)>{});
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        state_->awaiting_coroutine_ = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor_ = root->get_executor();
        }

        launch_all(std::make_index_sequence<sizeof...(Awaitables)>{});

        if (state_->completed.load(std::memory_order_acquire)) {
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
        for (auto& token : state_->cancellation_tokens) {
            if (token) token->store(true, std::memory_order_release);
        }
        std::apply(
            [](auto&... a) {
                (
                    [&](auto& aw) {
                        if constexpr (requires { aw.detach(); }) {
                            aw.detach();
                        }
                    }(a),
                    ...);
            },
            state_->awaitables_);

        if (state_->exception) {
            rethrow_and_clear(state_->exception);
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
// Timeout support (future enhancement)
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
 */
template <typename Duration>
class TimeoutAwaitable {
   private:
    Duration duration_;
    TimerService* timer_service_;
    std::atomic<bool> completed_{false};
    std::shared_ptr<std::atomic<bool>> timed_out_;
    uint64_t timer_id_{0};

   public:
    using result_type = void;

    explicit TimeoutAwaitable(Duration duration, TimerService* timer_service)
        : duration_(duration),
          timer_service_(timer_service),
          timed_out_(std::make_shared<std::atomic<bool>>(false)) {}

    ~TimeoutAwaitable() {
        completed_.store(true, std::memory_order_release);
        if (timer_service_ && timer_id_ != 0) {
            timer_service_->cancel_timeout(timer_id_);
        }
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        if (!timer_service_) {
            throw DFTUtilsException(
                ErrorCode::PIPELINE,
                "TimeoutAwaitable: TimerService not available");
        }

        timer_id_ = timer_service_->register_timeout(duration_, [this, h]() {
            if (!completed_.load(std::memory_order_acquire)) {
                timed_out_->store(true, std::memory_order_release);
                h.resume();
            }
        });
    }

    void await_resume() {
        completed_.store(true, std::memory_order_release);

        if (timed_out_->load(std::memory_order_acquire)) {
            throw DFTUtilsException(ErrorCode::PIPELINE, "Operation timed out");
        }
    }

    std::shared_ptr<std::atomic<bool>> get_cancellation_token() const {
        return timed_out_;
    }
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

#endif  // DFTRACER_UTILS_CORE_CORO_WHEN_ANY_H
