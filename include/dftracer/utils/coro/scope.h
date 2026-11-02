// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_SCOPE_H
#define DFTRACER_UTILS_CORO_SCOPE_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/join_handle.h>
#include <dftracer/utils/coro/spawn_future.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/**
 * @brief A counting scope (P3149) that runs tasks on one executor and joins
 * them.
 *
 * A scope that spawned must be joined with `co_await scope.join()` before it
 * is destroyed, or the process terminates. Code that can raise between a
 * spawn and the join uses @ref with_scope, which always joins. The first error
 * of a @ref spawn child is raised by @ref join and requests stop. The error of
 * a @ref spawn_future child goes only to its future, and cancelling that
 * future ends the child as cancelled at its next wait.
 *
 * Locals of a @ref with_scope body die when the body raises, before the join.
 * A child that uses one must end first, so the body awaits such children
 * before it can raise, or the local lives outside the body.
 *
 * A spawn after @ref close, or on an executor that is shutting down, does not
 * run the task and makes @ref join raise. A task spawned in this scope must
 * not join it, and @ref join raises when it does. Children may spawn into the
 * scope while it joins, and nothing may spawn after the join.
 */
class Scope {
   public:
    explicit Scope(Executor* executor = Executor::current()) noexcept
        : executor_(executor) {}
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    void spawn(CoroTask<void> task);

    /// Calls `fn()` inside the child's own frame, which owns `fn`, so a
    /// lambda's captures live as long as the task it returns.
    template <class Fn>
        requires std::is_invocable_r_v<CoroTask<void>, Fn&>
    void spawn(Fn fn) {
        if (!admit()) return;
        Coro child = run_fn_child(std::move(fn), this);
        start(child);
    }

    template <class T>
    SpawnFuture<T> spawn_future(CoroTask<T> task) {
        auto state = std::make_shared<SharedState<T>>();
        state->executor = executor_;
        if (std::exception_ptr refused = refusal()) {
            state->complete_with_exception(std::move(refused));
            return SpawnFuture<T>(std::move(state));
        }
        bind(task.handle().promise());
        task.handle().promise().cancel_source_ = &state->cancel_flag;
        Coro child = run_future_child(std::move(task), state);
        start(child);
        return SpawnFuture<T>(std::move(state));
    }

    template <class Fn,
              class T = typename std::invoke_result_t<Fn&>::value_type>
    SpawnFuture<T> spawn_future(Fn fn) {
        auto state = std::make_shared<SharedState<T>>();
        state->executor = executor_;
        if (std::exception_ptr refused = refusal()) {
            state->complete_with_exception(std::move(refused));
            return SpawnFuture<T>(std::move(state));
        }
        Coro child = run_fn_future_child(std::move(fn), this, state);
        start(child);
        return SpawnFuture<T>(std::move(state));
    }

    struct JoinAwaiter {
        static constexpr bool CANCEL_EXEMPT = true;

        Scope* scope;
        bool self_join{false};

        bool await_ready() const noexcept { return scope->joined_; }

        template <class Promise>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> awaiting) noexcept {
            if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
                if (awaiting.promise().get_root_promise()->scope_ == scope) {
                    self_join = true;
                    return awaiting;
                }
            }
            JoinHandle::JoinAwaitable inner = scope->join_.join();
            return inner.await_suspend(awaiting);
        }

        void await_resume() { scope->finish_join(self_join); }
    };

    /// Waits for every child, then raises the first @ref spawn error and
    /// closes the scope. A second join returns at once.
    [[nodiscard]] JoinAwaiter join() noexcept { return JoinAwaiter{this}; }

    /// Refuses later spawns. Children already spawned keep running.
    void close() noexcept { closed_.store(true, std::memory_order_release); }

    /// Sets the stop flag that children read through @ref stop_requested.
    void request_stop() noexcept {
        stop_.store(true, std::memory_order_release);
    }

    /// True after @ref request_stop, a child error, or once the executor
    /// begins to shut down.
    bool stop_requested() const noexcept;

    Executor* executor() const noexcept { return executor_; }

   private:
    bool admit();
    std::exception_ptr refusal() const;
    void fail(std::exception_ptr error) noexcept;
    void finish_join(bool self_join);
    void start(Coro& child);

    void bind(PromiseBase& promise) const noexcept {
        promise.set_executor(executor_);
        promise.scope_ = this;
    }

    static Coro run_child(CoroTask<void> task, Scope* scope);

    template <class Fn>
    static Coro run_fn_child(Fn fn, Scope* scope) {
        std::exception_ptr error;
        try {
            CoroTask<void> task = fn();
            scope->bind(task.handle().promise());
            co_await std::move(task);
        } catch (...) {
            error = std::current_exception();
        }
        if (error) scope->fail(std::move(error));
    }

    // Publishes an error only after the handler ends, since the awaiter may
    // run at once and the handler still holds the thrown object.
    template <class T>
    static Coro run_future_child(CoroTask<T> task,
                                 std::shared_ptr<SharedState<T>> state) {
        std::exception_ptr error;
        try {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(task);
                state->complete();
            } else {
                T value = co_await std::move(task);
                state->complete(std::move(value));
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error) state->complete_with_exception(std::move(error));
    }

    template <class Fn, class T>
    static Coro run_fn_future_child(Fn fn, Scope* scope,
                                    std::shared_ptr<SharedState<T>> state) {
        std::exception_ptr error;
        try {
            CoroTask<T> task = fn();
            scope->bind(task.handle().promise());
            task.handle().promise().cancel_source_ = &state->cancel_flag;
            if constexpr (std::is_void_v<T>) {
                co_await std::move(task);
                state->complete();
            } else {
                T value = co_await std::move(task);
                state->complete(std::move(value));
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error) state->complete_with_exception(std::move(error));
    }

    Executor* executor_;
    JoinHandle join_;
    std::exception_ptr error_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> spawned_{false};
    bool joined_{false};
};

namespace detail {
template <class T>
struct ScopeResult {
    dstd::Optional<T> value;
    void set(T v) { value.emplace(std::move(v)); }
    T take() { return std::move(*value); }
};
template <>
struct ScopeResult<void> {};
}  // namespace detail

/**
 * @brief Runs `fn(scope)` with a new @ref Scope on the current executor and
 * always joins it, also when `fn` raises.
 *
 * An error of `fn` requests stop before the join and is raised after it. A
 * child error is raised when `fn` had none.
 */
template <class Fn,
          class R = typename std::invoke_result_t<Fn&, Scope&>::value_type>
CoroTask<R> with_scope(Fn fn) {
    co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    Scope scope(executor);
    detail::ScopeResult<R> result;
    std::exception_ptr error;
    try {
        if constexpr (std::is_void_v<R>) {
            co_await fn(scope);
        } else {
            R value = co_await fn(scope);
            result.set(std::move(value));
        }
    } catch (...) {
        error = std::current_exception();
    }
    if (error) scope.request_stop();
    std::exception_ptr join_error;
    try {
        co_await scope.join();
    } catch (...) {
        join_error = std::current_exception();
    }
    if (error) common::rethrow_and_clear(error);
    if (join_error) common::rethrow_and_clear(join_error);
    if constexpr (!std::is_void_v<R>) co_return result.take();
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_SCOPE_H
