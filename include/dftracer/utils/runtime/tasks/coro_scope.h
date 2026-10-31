#ifndef DFTRACER_UTILS_RUNTIME_TASKS_CORO_SCOPE_H
#define DFTRACER_UTILS_RUNTIME_TASKS_CORO_SCOPE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/runtime/coro/async_generator.h>
#include <dftracer/utils/runtime/coro/channel.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/generator.h>
#include <dftracer/utils/runtime/coro/join_handle.h>
#include <dftracer/utils/runtime/coro/spawn_future.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/executor/executor.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace dftracer::utils {

namespace detail {
struct CoroScopeAccess;
}  // namespace detail

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
    std::atomic<bool> spawned_{false};
    std::atomic<bool> joined_{false};

    /// Cancellation: each scope has its own flag, and a scope also counts as
    /// cancelled while any scope it was spawned from is. A root scope also
    /// watches its executor's shutdown token. One allocation per scope.
    struct CancelChain {
        std::atomic<bool> flag{false};
        std::shared_ptr<std::atomic<bool>> external;
        std::shared_ptr<const CancelChain> parent;
    };

    static std::shared_ptr<CancelChain> make_chain(
        std::shared_ptr<std::atomic<bool>> external,
        std::shared_ptr<const CancelChain> parent) {
        auto chain = std::make_shared<CancelChain>();
        chain->external = std::move(external);
        chain->parent = std::move(parent);
        return chain;
    }

    std::shared_ptr<CancelChain> cancel_chain_{
        make_chain(executor_ ? executor_->shutdown_token() : nullptr, nullptr)};

    /// Internal: create a Coro wrapper, track it, and enqueue it.
    /// Returns the Coro's coroutine_handle for callers that need it.
    void enqueue_coro(coro::Coro& c) {
        if (!executor_->is_running()) {
            throw DFTUtilsException(
                ErrorCode::Pipeline,
                "Cannot spawn coroutine: executor is not running");
        }
        join_handle_.track(c);
        spawned_.store(true, std::memory_order_release);
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

    explicit CoroScope(Executor* executor) : executor_(executor) {}

    /// Child scope of a spawn, running on the chain made for it.
    CoroScope(Executor* executor, std::shared_ptr<CancelChain> chain)
        : executor_(executor), cancel_chain_(std::move(chain)) {}

    // A friend struct, not the constrained run_coro_scope template:
    // AppleClang 16 does not match a constrained friend template with its
    // definition, so the friend would not get access.
    friend struct detail::CoroScopeAccess;

   public:
    /// A scope for an owner that outlives the call that made it and joins it
    /// itself (a writer's member). Everything its children touch must live in
    /// heap state the owner holds by pointer, and the owner's destructor must
    /// join the scope off the executor thread or hand it to a joiner task.
    /// The long_lived_scopes ctest lists the callers.
    static std::unique_ptr<CoroScope> long_lived(
        Executor* executor = Executor::current()) {
        return std::unique_ptr<CoroScope>(new CoroScope(executor));
    }

    ~CoroScope() {
        if (!joined_.load(std::memory_order_acquire) &&
            spawned_.load(std::memory_order_acquire)) {
            DFTRACER_UTILS_LOG_ERROR(
                "CoroScope destroyed without join() after spawning children");
            std::terminate();
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
        auto chain = make_chain(nullptr, cancel_chain_);
        state->cancel_token =
            std::shared_ptr<std::atomic<bool>>(chain, &chain->flag);

        auto make_coro =
            [](auto f, Executor* exec, std::shared_ptr<CancelChain> own_chain,
               std::shared_ptr<coro::SharedState<void>> st) -> coro::Coro {
            // Each spawned coroutine gets its own CoroScope (child) with its
            // own cancel flag, so cancelling one child leaves its siblings.
            CoroScope child_scope(exec, std::move(own_chain));
            // Publish an exception only after the handler ends: the awaiter
            // may run at once, and the handler still holds the thrown object.
            std::exception_ptr error;
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
                error = std::current_exception();
            }
            if (error) st->complete_with_exception(std::move(error));
            co_await child_scope.join();
        };
        auto c = make_coro(std::forward<Func>(func), executor_,
                           std::move(chain), state);
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
        auto chain = make_chain(nullptr, cancel_chain_);
        state->cancel_token =
            std::shared_ptr<std::atomic<bool>>(chain, &chain->flag);

        auto make_coro =
            [](auto f, Executor* exec, std::shared_ptr<CancelChain> own_chain,
               std::shared_ptr<coro::SharedState<R>> st) -> coro::Coro {
            CoroScope child_scope(exec, std::move(own_chain));
            std::exception_ptr error;
            try {
                auto task = f(child_scope);
                task.handle().promise().set_executor(exec);
                R result = co_await std::move(task);
                st->complete(std::move(result));
            } catch (...) {
                error = std::current_exception();
            }
            if (error) st->complete_with_exception(std::move(error));
            co_await child_scope.join();
        };

        auto c = make_coro(std::forward<Func>(func), executor_,
                           std::move(chain), state);
        enqueue_coro(c);
        return coro::SpawnFuture<R>(std::move(state));
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
                const auto send_ok = co_await ch.send(std::move(item));
                if (!send_ok) {
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
                const auto send_ok = co_await ch.send(std::move(item));
                if (!send_ok) {
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

            for (;;) {
                auto item = co_await gen.next();
                if (!item) break;
                const auto send_ok = co_await ch.send(std::move(*item));
                if (!send_ok) {
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

            for (;;) {
                auto item = co_await gen.next();
                if (!item) break;
                const auto send_ok = co_await ch.send(std::move(*item));
                if (!send_ok) {
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
                for (;;) {
                    auto item = co_await channel.receive();
                    if (!item) break;
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
                for (;;) {
                    auto item = co_await ch.receive();
                    if (!item) break;
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
                for (;;) {
                    auto item = co_await input->receive();
                    if (!item) break;
                    auto result = co_await func(scope, std::move(*item));
                    const auto send_ok = co_await op.send(std::move(result));
                    if (!send_ok) {
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
                for (;;) {
                    auto item = co_await input.receive();
                    if (!item) break;
                    auto result = co_await func(scope, std::move(*item));
                    const auto send_ok = co_await op.send(std::move(result));
                    if (!send_ok) {
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

    // ====================================================================
    // Accessor / Utility
    // ====================================================================

    Executor* get_executor() const { return executor_; }

    /// Create a child scope, run the lambda, and auto-join.
    /// Compatibility bridge for code using ctx.coro_scope(...).
    template <typename Func>
        requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&>
    coro::CoroTask<void> coro_scope(Func&& scope_func) {
        co_await coro::detail::OWNS_SCOPE;
        CoroScope child(executor_);
        std::exception_ptr error;
        try {
            co_await scope_func(child);
        } catch (...) {
            error = std::current_exception();
        }
        co_await child.join();
        if (error) {
            rethrow_and_clear(error);
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

    /// True once this scope's token or that of any scope it was spawned from
    /// is set.
    bool is_cancellation_requested() const {
        for (const CancelChain* c = cancel_chain_.get(); c;
             c = c->parent.get()) {
            if (c->flag.load(std::memory_order_acquire)) return true;
            if (c->external && c->external->load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /// This scope's own token. Setting it cancels this scope and the scopes
    /// spawned from it, not its parent or siblings.
    std::shared_ptr<std::atomic<bool>> get_cancellation_token() const {
        return std::shared_ptr<std::atomic<bool>>(cancel_chain_,
                                                  &cancel_chain_->flag);
    }
};

// ========================================================================
// Standalone coro_scope() helper
// Creates a child CoroScope, runs the lambda, and auto-joins.
// ========================================================================

namespace detail {
struct CoroScopeAccess {
    static CoroScope root(Executor* executor) { return CoroScope(executor); }
};
}  // namespace detail

template <typename Func, typename... Args>
    requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&,
                                   Args...>
inline coro::CoroTask<void> run_coro_scope(Executor* executor, Func scope_func,
                                           Args... args) {
    co_await coro::detail::OWNS_SCOPE;
    CoroScope scope = detail::CoroScopeAccess::root(executor);
    std::exception_ptr error;
    try {
        co_await scope_func(scope, std::move(args)...);
    } catch (...) {
        error = std::current_exception();
    }
    co_await scope.join();
    if (error) {
        rethrow_and_clear(error);
    }
    co_return;
}

/// Runs `task` as a detached coroutine on `executor`. False, and `task` never
/// runs, when the executor is null or not running.
inline bool spawn_detached(Executor* executor, coro::CoroTask<void> task) {
    if (!executor || !executor->is_running()) return false;
    auto make_coro = [](coro::CoroTask<void> t, Executor* exec) -> coro::Coro {
        try {
            t.handle().promise().set_executor(exec);
            co_await std::move(t);
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR("detached task failed");
        }
    };
    auto c = make_coro(std::move(task), executor);
    c.handle().promise().executor = executor;
    auto handle = c.release();
    executor->enqueue(handle);
    return true;
}

/// Same, on the executor already driving this thread. Lets a utility fan
/// out without naming an executor or a runtime.
template <typename Func, typename... Args>
    requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&,
                                   Args...>
inline coro::CoroTask<void> run_coro_scope(Func scope_func, Args... args) {
    return run_coro_scope(Executor::current(), std::move(scope_func),
                          std::move(args)...);
}

namespace detail {
template <typename T>
struct with_scope_value;
template <typename T>
struct with_scope_value<coro::CoroTask<T>> {
    using type = T;
};
}  // namespace detail

/**
 * @brief Invoke a scope-taking op with a fresh scope: the dependency-injection
 * seam for context ops.
 *
 * A context op is `operator()(CoroScope&, In) -> CoroTask<Out>`: a caller
 * injects its own scope so the op composes into a larger fan-out. with_scope is
 * the standalone contract - it opens a CoroScope on the current executor, runs
 * the op, and joins - so an op can offer a scope-less overload:
 * @code
 * coro::CoroTask<Out> operator()(const In& in) const {
 *     return with_scope(*this, in);
 * }
 * @endcode
 *
 * @p Out must be default-constructible. Requires a current executor.
 */
template <typename Op, typename In>
coro::CoroTask<typename detail::with_scope_value<
    std::invoke_result_t<const Op&, CoroScope&, const In&>>::type>
with_scope(const Op& op, In in) {
    typename detail::with_scope_value<
        std::invoke_result_t<const Op&, CoroScope&, const In&>>::type result;
    co_await run_coro_scope(
        Executor::current(),
        [&op, &in, &result](CoroScope& s) -> coro::CoroTask<void> {
            result = co_await op(s, in);
        });
    co_return result;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_TASKS_CORO_SCOPE_H
