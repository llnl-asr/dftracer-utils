#ifndef DFTRACER_UTILS_CORE_TASKS_CORO_SCOPE_H
#define DFTRACER_UTILS_CORE_TASKS_CORO_SCOPE_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/generator.h>
#include <dftracer/utils/core/coro/join_handle.h>
#include <dftracer/utils/core/coro/spawn_future.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/core/utilities/utility_traits.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils {

/// Lightweight structured concurrency scope using Coro + JoinHandle.
///
/// This is the single context type that task lambdas receive.
/// It replaces both CoroScope and CoroScope with a unified API:
/// - spawn() returning SpawnFuture<T> for all coroutines (void and typed)
/// - io::async_read/write/open/close for async I/O
/// - receive() for channel consumption
/// - Cancellation support
///
/// All spawned work items are lightweight Coro instances enqueued
/// directly to the Executor's run_queue_ -- no Task objects, no
/// Scheduler overhead, no when_all().
///
/// spawn() always returns SpawnFuture<T>. For void coroutines the
/// return value can be ignored (fire-and-forget) or co_await'd to
/// wait for that specific coroutine to complete.
///
/// Usage:
/// @code
/// auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
///     // Fire-and-forget (return value ignored):
///     scope.spawn([](CoroScope& s) -> coro::CoroTask<void> {
///         co_return;
///     });
///
///     // Await a void spawn:
///     co_await scope.spawn([](CoroScope& s) -> coro::CoroTask<void> {
///         // do work
///         co_return;
///     });
///
///     // Await a typed spawn:
///     int result = co_await scope.spawn(
///         [](CoroScope& s) -> coro::CoroTask<int> {
///             co_return 42;
///         });
///     co_return;
/// });
/// @endcode
class CoroScope {
   private:
    Executor* executor_;
    coro::JoinHandle join_handle_;
    std::vector<coro::Coro> coroutines_;
    std::atomic<bool> joined_{false};

    // Cancellation support
    std::shared_ptr<std::atomic<bool>> cancellation_requested_{
        std::make_shared<std::atomic<bool>>(false)};

    /// Internal: create a Coro wrapper, track it, and enqueue it.
    /// Returns the Coro's coroutine_handle for callers that need it.
    void enqueue_coro(coro::Coro& c) {
        if (!executor_->is_running()) {
            throw std::runtime_error(
                "Cannot spawn coroutine: executor is not running");
        }
        join_handle_.track(c);
        c.handle().promise().executor = executor_;
        auto handle = c.handle();
        // Transfer ownership: set released=true BEFORE enqueueing so
        // FinalAwaiter will schedule deferred destruction via the
        // worker's thread-local destroy list.  This avoids the race
        // where another worker frees the frame while the coroutine-
        // suspend machinery still needs it.
        (void)c.release();
        executor_->enqueue(handle);
    }

   public:
    explicit CoroScope(Executor* executor) : executor_(executor) {}

    /// Construct with an inherited cancellation token (for child scopes
    /// spawned from a parent -- shares the parent's cancellation signal).
    CoroScope(Executor* executor,
              std::shared_ptr<std::atomic<bool>> cancellation_token)
        : executor_(executor),
          cancellation_requested_(std::move(cancellation_token)) {}

    ~CoroScope() {
        if (!joined_ && !coroutines_.empty()) {
            DFTRACER_UTILS_LOG_WARN(
                "CoroScope destroyed without join()! "
                "%zu coroutines still pending -- releasing to executor.",
                coroutines_.size());
            // Release un-joined coroutines so the executor can manage their
            // lifetime. Without this, destroying the Coro objects would destroy
            // the coroutine frames while workers may still be executing them
            // (use-after-free). release() sets the 'released' flag so that
            // FinalAwaiter schedules deferred destruction via the executor.
            for (auto& c : coroutines_) {
                if (!c.done()) {
                    // Detach from join group before releasing -- the
                    // JoinHandle lives in this scope and is about to be
                    // destroyed. Nulling the pointers prevents FinalAwaiter
                    // from accessing the destroyed counter/continuation.
                    auto& p = c.handle().promise();
                    p.join_counter = nullptr;
                    p.join_continuation = nullptr;
                    (void)c.release();
                }
            }
        }
    }

    CoroScope(const CoroScope&) = delete;
    CoroScope& operator=(const CoroScope&) = delete;
    CoroScope(CoroScope&&) = delete;
    CoroScope& operator=(CoroScope&&) = delete;

    // ====================================================================
    // Basic Spawning (void)
    // ====================================================================

    /// Spawn a void coroutine on the executor's run queue.
    ///
    /// The lambda receives CoroScope& and returns CoroTask<void>.
    /// Internally wrapped in a lightweight Coro and enqueued directly.
    ///
    /// Returns SpawnFuture<void> that can be co_await'd to wait for
    /// this specific coroutine to complete. The return value can be
    /// safely ignored for fire-and-forget usage.
    ///
    /// The captureless-lambda-with-parameters pattern ensures
    /// coroutine parameters are copied into the coroutine frame,
    /// avoiding the dangling-capture bug.
    ///
    /// Usage:
    /// @code
    /// // Fire-and-forget (existing usage, still works):
    /// scope.spawn([](CoroScope& s) -> coro::CoroTask<void> {
    ///     co_return;
    /// });
    ///
    /// // Awaitable (new):
    /// co_await scope.spawn([](CoroScope& s) -> coro::CoroTask<void> {
    ///     // do work
    ///     co_return;
    /// });
    /// @endcode
    template <typename Func,
              typename R =
                  typename std::invoke_result_t<Func, CoroScope&>::value_type,
              std::enable_if_t<std::is_void_v<R>, int> = 0>
    coro::SpawnFuture<void> spawn(Func&& func) {
        auto state = std::make_shared<coro::SharedState<void>>();
        state->executor = executor_;

        auto make_coro =
            [](auto f, Executor* exec,
               std::shared_ptr<std::atomic<bool>> cancel_token,
               std::shared_ptr<coro::SharedState<void>> st) -> coro::Coro {
            // Each spawned coroutine gets its own CoroScope (child) so it
            // can safely outlive the parent scope (e.g., when_any completes
            // while slow tasks are still running). Shares cancellation.
            CoroScope child_scope(exec, std::move(cancel_token));
            try {
                auto task = f(child_scope);
                // Propagate executor to the CoroTask's PromiseBase
                // so channel send/receive can schedule resumptions.
                // CoroPromise is not a PromiseBase, so we must do this
                // manually.
                task.handle().promise().set_executor(exec);
                co_await std::move(task);
                st->complete();
            } catch (...) {
                auto ex = std::current_exception();
                st->complete_with_exception(std::move(ex));
            }
            co_await child_scope.join();
        };
        auto c = make_coro(std::forward<Func>(func), executor_,
                           cancellation_requested_, state);
        enqueue_coro(c);
        return coro::SpawnFuture<void>(std::move(state));
    }

    // ====================================================================
    // Typed Spawning (returning SpawnFuture<T>)
    // ====================================================================

    /// Spawn a coroutine that returns a typed result.
    ///
    /// Returns SpawnFuture<T> that can be co_await'd to retrieve
    /// the result. One heap allocation (shared_ptr<SharedState<T>>).
    ///
    /// Usage:
    /// @code
    /// auto future = scope.spawn([](CoroScope& s) -> coro::CoroTask<int> {
    ///     co_return 42;
    /// });
    /// int val = co_await future;
    /// @endcode
    template <typename Func,
              typename R =
                  typename std::invoke_result_t<Func, CoroScope&>::value_type,
              std::enable_if_t<!std::is_void_v<R>, int> = 0>
    coro::SpawnFuture<R> spawn(Func&& func) {
        auto state = std::make_shared<coro::SharedState<R>>();
        state->executor = executor_;

        auto make_coro =
            [](auto f, Executor* exec,
               std::shared_ptr<std::atomic<bool>> cancel_token,
               std::shared_ptr<coro::SharedState<R>> st) -> coro::Coro {
            CoroScope child_scope(exec, std::move(cancel_token));
            try {
                auto task = f(child_scope);
                task.handle().promise().set_executor(exec);
                R result = co_await std::move(task);
                st->complete(std::move(result));
            } catch (...) {
                auto ex = std::current_exception();
                st->complete_with_exception(std::move(ex));
            }
            co_await child_scope.join();
        };

        auto c = make_coro(std::forward<Func>(func), executor_,
                           cancellation_requested_, state);
        enqueue_coro(c);
        return coro::SpawnFuture<R>(std::move(state));
    }

    template <typename UtilityT, typename InputT,
              typename DecayedUtility = std::remove_reference_t<UtilityT>,
              typename R = typename DecayedUtility::Output,
              std::enable_if_t<
                  utilities::detail::has_process_v<DecayedUtility, InputT, R>,
                  int> = 0>
    coro::SpawnFuture<R> spawn(UtilityT& utility, InputT input) {
        return spawn([utility_ptr = &utility, input = std::move(input)](
                         CoroScope& child_scope) mutable -> coro::CoroTask<R> {
            if constexpr (utilities::has_tag_v<utilities::tags::NeedsContext,
                                               DecayedUtility>) {
                utility_ptr->set_context(child_scope);
                try {
                    R result = co_await utility_ptr->process(input);
                    utility_ptr->clear_context();
                    co_return result;
                } catch (...) {
                    utility_ptr->clear_context();
                    throw;
                }
            } else {
                co_return co_await utility_ptr->process(input);
            }
        });
    }

    // ====================================================================
    // Channel Operations
    // ====================================================================

    /// Async receive from channel.
    template <typename T>
    auto receive(coro::Channel<T>& channel) {
        return channel.receive();
    }

    /// Async receive from channel (shared_ptr).
    template <typename T>
    auto receive(std::shared_ptr<coro::Channel<T>> channel) {
        return channel->receive();
    }

    // ====================================================================
    // Producer-Consumer Convenience Wrappers
    // ====================================================================

    /// Spawn producer feeding Generator<T> into Channel<T> (reference).
    template <typename T, typename Func>
    void spawn_producer(coro::Channel<T>& channel, Func&& generator_func) {
        spawn([ch = channel.producer(),
               func = std::forward<Func>(generator_func)](
                  CoroScope& scope) mutable -> coro::CoroTask<void> {
            auto guard = ch.guard();
            coro::Generator<T> gen = func(scope);

            for (auto item : gen) {
                if (!co_await ch.send(std::move(item))) {
                    co_return;
                }
            }

            co_return;
        });
    }

    /// Spawn producer feeding Generator<T> into Channel<T> (shared_ptr).
    template <typename T, typename Func>
    void spawn_producer(std::shared_ptr<coro::Channel<T>> channel,
                        Func&& generator_func) {
        spawn([ch = channel->producer(),
               func = std::forward<Func>(generator_func)](
                  CoroScope& scope) mutable -> coro::CoroTask<void> {
            auto guard = ch.guard();
            coro::Generator<T> gen = func(scope);

            for (auto item : gen) {
                if (!co_await ch.send(std::move(item))) {
                    co_return;
                }
            }

            co_return;
        });
    }

    /// Spawn async producer feeding AsyncGenerator<T> into Channel<T>
    /// (reference).
    template <typename T, typename Func>
    void spawn_async_producer(coro::Channel<T>& channel,
                              Func&& async_generator_func) {
        spawn([ch = channel.producer(),
               func = std::forward<Func>(async_generator_func)](
                  CoroScope& scope) mutable -> coro::CoroTask<void> {
            auto guard = ch.guard();
            coro::AsyncGenerator<T> gen = func(scope);

            while (auto item = co_await gen.next()) {
                if (!co_await ch.send(std::move(*item))) {
                    co_return;
                }
            }

            co_return;
        });
    }

    /// Spawn async producer feeding AsyncGenerator<T> into Channel<T>
    /// (shared_ptr).
    template <typename T, typename Func>
    void spawn_async_producer(std::shared_ptr<coro::Channel<T>> channel,
                              Func&& async_generator_func) {
        spawn([ch = channel->producer(),
               func = std::forward<Func>(async_generator_func)](
                  CoroScope& scope) mutable -> coro::CoroTask<void> {
            auto guard = ch.guard();
            coro::AsyncGenerator<T> gen = func(scope);

            while (auto item = co_await gen.next()) {
                if (!co_await ch.send(std::move(*item))) {
                    co_return;
                }
            }

            co_return;
        });
    }

    /// Spawn N producers (shared_ptr).
    template <typename T, typename Func>
    void spawn_producers(std::shared_ptr<coro::Channel<T>> channel,
                         std::size_t count, Func&& producer_func) {
        for (std::size_t i = 0; i < count; ++i) {
            spawn([ch = channel->producer(), func = producer_func,
                   i](CoroScope& scope) mutable -> coro::CoroTask<void> {
                auto guard = ch.guard();
                co_await func(scope, i);
                co_return;
            });
        }
    }

    /// Spawn N producers (reference).
    template <typename T, typename Func>
    void spawn_producers(coro::Channel<T>& channel, std::size_t count,
                         Func&& producer_func) {
        for (std::size_t i = 0; i < count; ++i) {
            spawn([ch = channel.producer(), func = producer_func,
                   i](CoroScope& scope) mutable -> coro::CoroTask<void> {
                auto guard = ch.guard();
                co_await func(scope, i);
                co_return;
            });
        }
    }

    /// Spawn N consumers draining Channel<T> (reference).
    template <typename T, typename Func>
    void spawn_consumers(coro::Channel<T>& channel, std::size_t count,
                         Func&& consumer_func) {
        for (std::size_t i = 0; i < count; i++) {
            spawn([&channel, func = consumer_func](
                      CoroScope& scope) -> coro::CoroTask<void> {
                while (auto item = co_await channel.receive()) {
                    co_await func(scope, std::move(*item));
                }
                co_return;
            });
        }
    }

    /// Spawn N consumers draining Channel<T> (shared_ptr).
    template <typename T, typename Func>
    void spawn_consumers(std::shared_ptr<coro::Channel<T>> channel,
                         std::size_t count, Func&& consumer_func) {
        for (std::size_t i = 0; i < count; i++) {
            spawn([ch = channel->consumer(), func = consumer_func](
                      CoroScope& scope) -> coro::CoroTask<void> {
                while (auto item = co_await ch.receive()) {
                    co_await func(scope, std::move(*item));
                }
                co_return;
            });
        }
    }

    // ====================================================================
    // Transform (Consumer-Producer Bridge)
    // ====================================================================

    /// Spawn N transform workers (shared_ptr).
    template <typename TIn, typename TOut, typename Func>
    void spawn_transforms(std::shared_ptr<coro::Channel<TIn>> input,
                          std::shared_ptr<coro::Channel<TOut>> output,
                          std::size_t count, Func&& transform_func) {
        for (std::size_t i = 0; i < count; ++i) {
            spawn([input, op = output->producer(), func = transform_func](
                      CoroScope& scope) mutable -> coro::CoroTask<void> {
                auto guard = op.guard();
                while (auto item = co_await input->receive()) {
                    auto result = co_await func(scope, std::move(*item));
                    if (!co_await op.send(std::move(result))) {
                        co_return;
                    }
                }
                co_return;
            });
        }
    }

    /// Spawn N transform workers (reference).
    template <typename TIn, typename TOut, typename Func>
    void spawn_transforms(coro::Channel<TIn>& input,
                          coro::Channel<TOut>& output, std::size_t count,
                          Func&& transform_func) {
        for (std::size_t i = 0; i < count; ++i) {
            spawn([&input, op = output.producer(), func = transform_func](
                      CoroScope& scope) mutable -> coro::CoroTask<void> {
                auto guard = op.guard();
                while (auto item = co_await input.receive()) {
                    auto result = co_await func(scope, std::move(*item));
                    if (!co_await op.send(std::move(result))) {
                        co_return;
                    }
                }
                co_return;
            });
        }
    }

    // ====================================================================
    // Waiting
    // ====================================================================

    /// Wait for all spawned coroutines to complete.
    /// Must be called before CoroScope is destroyed.
    /// Idempotent: calling join() a second time is a no-op.
    coro::CoroTask<void> join() {
        if (joined_.exchange(true, std::memory_order_acq_rel)) {
            co_return;
        }
        co_await join_handle_.join();
        // Frames were released in enqueue_coro() and will be
        // destroyed by workers via thread-local destroy lists
        // in FinalAwaiter.  Nothing to clean up here.
        co_return;
    }

    /// Alias for join() -- backward compatibility with old join_all() callers.
    coro::CoroTask<void> join_all() { return join(); }

    std::size_t size() const { return coroutines_.size(); }
    bool is_joined() const { return joined_; }

    // ====================================================================
    // Accessor / Utility
    // ====================================================================

    Executor* get_executor() const { return executor_; }

    /// Create a child scope, run the lambda, and auto-join.
    /// Compatibility bridge for code using ctx.coro_scope(...).
    template <typename Func>
        requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&>
    coro::CoroTask<void> coro_scope(Func&& scope_func) {
        CoroScope child(executor_);
        std::exception_ptr error;
        try {
            co_await scope_func(child);
        } catch (...) {
            error = std::current_exception();
        }
        co_await child.join();
        if (error) {
            auto ex = std::move(error);
            error = nullptr;
            std::rethrow_exception(std::move(ex));
        }
        co_return;
    }

    /// Compatibility bridge for code using ctx.scope(...).
    /// Same as coro_scope().
    template <typename Func>
        requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&>
    coro::CoroTask<void> scope(Func&& scope_func) {
        return coro_scope(std::forward<Func>(scope_func));
    }

    // ====================================================================
    // Cancellation Support
    // ====================================================================

    bool is_cancellation_requested() const {
        return cancellation_requested_->load(std::memory_order_acquire);
    }

    void request_cancellation() {
        cancellation_requested_->store(true, std::memory_order_release);
    }

    std::shared_ptr<std::atomic<bool>> get_cancellation_token() const {
        return cancellation_requested_;
    }
};

// ========================================================================
// Standalone coro_scope() helper
// Creates a child CoroScope, runs the lambda, and auto-joins.
// ========================================================================

template <typename Func, typename... Args>
    requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&,
                                   Args...>
inline coro::CoroTask<void> run_coro_scope(Executor* executor, Func scope_func,
                                           Args... args) {
    CoroScope scope(executor);
    std::exception_ptr error;
    try {
        co_await scope_func(scope, std::move(args)...);
    } catch (...) {
        error = std::current_exception();
    }
    co_await scope.join();
    if (error) {
        auto ex = std::move(error);
        error = nullptr;
        std::rethrow_exception(std::move(ex));
    }
    co_return;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_CORO_SCOPE_H
