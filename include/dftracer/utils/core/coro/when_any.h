#ifndef DFTRACER_UTILS_CORE_CORO_WHEN_ANY_H
#define DFTRACER_UTILS_CORE_CORO_WHEN_ANY_H

#include <dftracer/utils/core/coro/resumption_helper.h>
#include <dftracer/utils/core/coro/task.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// Timer service needed for TimeoutAwaitable
#include <dftracer/utils/core/common/timer_service.h>

namespace dftracer::utils::coro {

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
struct WhenAnySharedState {
    std::atomic<bool> completed{false};
    WhenAnyResult<typename Awaitable::result_type> result;
    std::exception_ptr exception;
    std::coroutine_handle<> awaiting_coroutine;
    std::vector<std::shared_ptr<std::atomic<bool>>> cancellation_tokens;
    std::vector<coro::CoroTask<void>> wrapper_tasks;
    std::atomic<std::size_t> wrappers_done{0};
    std::size_t total_wrappers{0};
    std::vector<Awaitable> awaitables;
    Executor* executor{nullptr};

    // Single-atomic coordination between await_suspend and wrapper
    // completion.  Uses fetch_or(acq_rel) on a bitmask -- the total
    // modification order on one atomic guarantees exactly one side sees the
    // other's bit, eliminating the store-buffer (SB) reordering hazard that
    // two independent atomics with seq_cst were guarding against.
    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> sync_state_{0};

    explicit WhenAnySharedState(std::vector<Awaitable> aws)
        : awaitables(std::move(aws)) {
        total_wrappers = awaitables.size();
        cancellation_tokens.reserve(awaitables.size());
        wrapper_tasks.reserve(awaitables.size());

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
        // resuming wrapper coroutine handles that are about to be destroyed.
        // This is critical: losing wrappers are suspended at co_await on these
        // awaitables, and destroying the wrappers would leave dangling handles
        // in the awaitables' shared state.
        for (auto& a : awaitables) {
            if constexpr (requires { a.detach(); }) {
                a.detach();
            }
        }
        // Clear wrapper_tasks to break circular reference:
        // wrappers hold shared_ptr<WhenAnySharedState> in their coroutine
        // frames, and this state holds wrapper_tasks containing those wrappers.
        wrapper_tasks.clear();
    }

    // Called by the first wrapper to complete (winner of CAS)
    void on_first_complete() {
        auto prev =
            sync_state_.fetch_or(BIT_COMPLETED, std::memory_order_acq_rel);
        if (prev & BIT_SUSPENDED) {
            if (awaiting_coroutine && !awaiting_coroutine.done()) {
                if (executor) {
                    schedule_coroutine_resumption_helper(executor,
                                                         awaiting_coroutine);
                } else {
                    awaiting_coroutine.resume();
                }
            }
        }
    }

    // Called by await_suspend after deciding to suspend but before returning
    void mark_suspended_and_check_completion() {
        auto prev =
            sync_state_.fetch_or(BIT_SUSPENDED, std::memory_order_acq_rel);
        if (prev & BIT_COMPLETED) {
            if (awaiting_coroutine && !awaiting_coroutine.done()) {
                if (executor) {
                    schedule_coroutine_resumption_helper(executor,
                                                         awaiting_coroutine);
                } else {
                    awaiting_coroutine.resume();
                }
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
 *     ctx.spawn_io([&]() { return read_fast_storage(); }),
 *     ctx.spawn_io([&]() { return read_slow_storage(); }),
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
        state_->awaiting_coroutine = h;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            state_->executor = root->get_executor();
        }

        if (state_->awaitables.empty()) {
            return false;
        }

        for (std::size_t i = 0; i < state_->awaitables.size(); i++) {
            launch_wrapper(i);
        }

        // Check if any task completed synchronously
        if (state_->completed.load(std::memory_order_acquire)) {
            return false;  // Don't suspend
        }

        // We will suspend - mark it and double-check for completion
        state_->mark_suspended_and_check_completion();

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->awaiting_async_ = true;
        }

        return true;
    }

    result_type await_resume() {
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return std::move(state_->result);
    }

   private:
    void launch_wrapper(std::size_t i) {
        auto wrapper = [](std::shared_ptr<SharedState> state,
                          std::size_t index) -> coro::CoroTask<void> {
            try {
                if (state->completed.load(std::memory_order_acquire)) {
                    state->wrappers_done.fetch_add(1,
                                                   std::memory_order_release);
                    co_return;
                }

                auto result = co_await state->awaitables[index];

                bool expected = false;
                if (state->completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    // We're the first to complete - store result
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

                    // Use double-check pattern for resumption
                    state->on_first_complete();
                }

                state->wrappers_done.fetch_add(1, std::memory_order_release);
            } catch (...) {
                if (state->completed.load(std::memory_order_acquire)) {
                    state->wrappers_done.fetch_add(1,
                                                   std::memory_order_release);
                    co_return;
                }

                bool expected = false;
                if (state->completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    // We're the first to complete (with exception)
                    try {
                        state->exception = std::current_exception();
                    } catch (...) {
                    }

                    // Use double-check pattern for resumption
                    state->on_first_complete();
                }

                state->wrappers_done.fetch_add(1, std::memory_order_release);
            }
            co_return;
        }(state_, i);

        wrapper.resume();
        state_->wrapper_tasks.push_back(std::move(wrapper));
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
 *     ctx.spawn_io([&]() { return read_cache(); }),
 *     ctx.spawn_io([&]() { return read_disk(); }),
 *     ctx.spawn_io([&]() { return read_network(); })
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
// Timeout support (future enhancement)
// ============================================================================

/**
 * TimeoutAwaitable - Awaitable that completes after a duration
 *
 * Can be used with when_any to implement timeouts:
 *
 * @code
 * auto result = co_await when_any({
 *     ctx.spawn_io([&]() { return read_file(); }),
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
            throw std::runtime_error(
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
            throw std::runtime_error("Operation timed out");
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
