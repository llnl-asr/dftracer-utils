// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_SCOPE_H
#define DFTRACER_UTILS_CORO_SCOPE_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/join_handle.h>
#include <dftracer/utils/coro/spawn_future.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/// What a child error does to its siblings. Both raise the first error from
/// @ref Scope::join. An error is an exception that code with exceptions on
/// threw in the child, since a task that returns an error result is not a
/// @ref Scope::spawn child.
enum class ScopeMode : std::uint8_t {
    /// Asks the siblings to stop, as @ref Scope::request_stop does.
    FailFast,
    /// Leaves the siblings running.
    Supervisor,
};

/**
 * @brief A counting scope (P3149) that runs tasks on one executor and joins
 * them.
 *
 * A scope that spawned must be joined with `co_await scope.join()` before it
 * is destroyed, or the process terminates. Code that can raise between a
 * spawn and the join uses @ref with_scope, which always joins. The first error
 * of a @ref spawn child is raised by @ref join and, in the default mode,
 * requests stop. The error of a @ref spawn_future child goes only to its
 * future, and cancelling that future ends the child as cancelled at its next
 * wait.
 *
 * A stop ends every child as cancelled at its next wait, and wakes one that is
 * parked. A child that stops is not an error of @ref join. A child that
 * stopped while the scope had no stop requested, as at shutdown, sets
 * @ref child_stopped, and @ref with_scope then stops its own task after the
 * join. A scope made by @ref with_scope also stops when the task that holds it
 * is cancelled, so scopes and tasks form a tree.
 *
 * Locals of a @ref with_scope body die when the body raises, before the join.
 * A child that uses one must end first, so the body awaits such children
 * before it can raise, or the local lives outside the body.
 *
 * A spawn on an executor that is shutting down does not run the task and
 * counts as a child that stopped. A function given to @ref spawn that throws
 * before it returns its task fails the child like any other error. A spawn
 * after @ref close or after the
 * join, a spawn without a running executor, and a task that joins the scope
 * that spawned it are misuse and end the process. Children may spawn into the
 * scope while it joins, and nothing may spawn after the join.
 */
class Scope {
   public:
    /// @param owner The promise of the frame that holds this scope, which must
    ///        reach the join. A cancel of that task stops the children.
    explicit Scope(Executor* executor = Executor::current(),
                   const PromiseBase* owner = nullptr,
                   ScopeMode mode = ScopeMode::FailFast) noexcept
        : executor_(executor), owner_(owner), mode_(mode) {}
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
        detail::Coro child = run_fn_child(std::move(fn), this);
        start(child);
    }

    template <class T>
    SpawnFuture<T> spawn_future(CoroTask<T> task) {
        auto state = dstd::make_shared<SharedState<T>>();
        state->executor = executor_;
        if (is_refused()) {
            state->complete_stopped();
            return SpawnFuture<T>(std::move(state));
        }
        bind(task.handle().promise());
        task.handle().promise().cancel_source = &state->cancel_flag;
        detail::Coro child = run_future_child(std::move(task), state);
        start(child);
        return SpawnFuture<T>(std::move(state));
    }

    template <class Fn,
              class T = typename std::invoke_result_t<Fn&>::value_type>
    SpawnFuture<T> spawn_future(Fn fn) {
        auto state = dstd::make_shared<SharedState<T>>();
        state->executor = executor_;
        if (is_refused()) {
            state->complete_stopped();
            return SpawnFuture<T>(std::move(state));
        }
        detail::Coro child = run_fn_future_child(std::move(fn), this, state);
        child.handle().promise().exception_sink = &future_sink<T>;
        child.handle().promise().sink_owner = state.get();
        start(child);
        return SpawnFuture<T>(std::move(state));
    }

    struct JoinAwaiter {
        static constexpr bool CANCEL_EXEMPT = true;

        Scope* scope;
        bool quiet{false};

        bool await_ready() const noexcept { return scope->joined_; }

        template <class Promise>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> awaiting) noexcept {
            if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
                if (awaiting.promise().get_root_promise()->scope == scope) {
                    dstd::fatal(
                        "coro: a task joined the scope that spawned it");
                }
            }
            JoinHandle::JoinAwaitable inner = scope->join_.join();
            return inner.await_suspend(awaiting);
        }

        // Raised here and not in the library, whose code has no way to unwind.
        void await_resume() {
            scope->finish_join();
            if (quiet) return;
            if (std::exception_ptr error = scope->take_error()) {
                common::rethrow_and_clear(error);
            }
        }
    };

    /// Waits for every child, then raises the first @ref spawn error and
    /// closes the scope. A second join returns at once.
    [[nodiscard]] JoinAwaiter join() noexcept { return JoinAwaiter{this}; }

    /// @ref join that raises nothing: the caller takes the error with
    /// @ref take_error. For code that must also handle an error of its own.
    [[nodiscard]] JoinAwaiter join_quietly() noexcept {
        return JoinAwaiter{this, true};
    }

    /// The first @ref spawn error, or null, and clears it.
    std::exception_ptr take_error() noexcept { return std::move(error_); }

    /// True after a child stopped while the scope had no stop requested.
    bool child_stopped() const noexcept {
        return child_stopped_.load(std::memory_order_acquire);
    }

    /// Refuses later spawns. Children already spawned keep running.
    void close() noexcept { closed_.store(true, std::memory_order_release); }

    /// Ends the children as cancelled at their next wait, and wakes those that
    /// are parked. A child ended by this is not an error of @ref join.
    void request_stop() noexcept;

    /// Stops the children too when @p token stops. Call before the first spawn.
    void link_stop(StopToken token) noexcept { linked_ = std::move(token); }

    /// True after @ref request_stop, a child error, or once the executor
    /// begins to shut down.
    bool stop_requested() const noexcept;

    Executor* executor() const noexcept { return executor_; }

   private:
    friend bool detail::scope_stopping(const Scope& scope) noexcept;

    bool admit();
    bool is_refused() const;
    void fail(std::exception_ptr error) noexcept;
    void child_failed(std::exception_ptr error) noexcept;
    static void child_sink(void* scope, std::exception_ptr error) noexcept;
    void note_child_stopped() noexcept;
    void finish_join() noexcept;
    void start(detail::Coro& child);

    void bind(PromiseBase& promise) const noexcept {
        promise.set_executor(executor_);
        promise.scope = this;
    }

    static detail::Coro run_child(CoroTask<void> task, Scope* scope);

    template <class Fn>
    static detail::Coro run_fn_child(Fn fn, Scope* scope) {
        CoroTask<void> task = fn();
        scope->bind(task.handle().promise());
        auto outcome = co_await settle(std::move(task));
        scope->finish_child(std::move(outcome));
    }

    // Where an exception goes that ends a child before its task exists.
    template <class T>
    static void future_sink(void* state, std::exception_ptr error) noexcept {
        static_cast<SharedState<T>*>(state)->complete_with_exception(
            std::move(error));
    }

    template <class T>
    static detail::Coro run_future_child(
        CoroTask<T> task, dstd::SharedPtr<SharedState<T>> state) {
        auto outcome = co_await settle(std::move(task));
        publish(*state, std::move(outcome));
    }

    template <class Fn, class T>
    static detail::Coro run_fn_future_child(
        Fn fn, Scope* scope, dstd::SharedPtr<SharedState<T>> state) {
        CoroTask<T> task = fn();
        scope->bind(task.handle().promise());
        task.handle().promise().cancel_source = &state->cancel_flag;
        auto outcome = co_await settle(std::move(task));
        publish(*state, std::move(outcome));
    }

    void finish_child(detail::Settled<void> outcome) noexcept {
        if (outcome.exception) {
            child_failed(std::move(outcome.exception));
        } else if (outcome.stopped) {
            note_child_stopped();
        }
    }

    // Publishes the outcome from the frame body, after the settled task is
    // gone, since the awaiter may run at once.
    template <class T>
    static void publish(SharedState<T>& state,
                        detail::Settled<T> outcome) noexcept {
        if (outcome.exception) {
            state.complete_with_exception(std::move(outcome.exception));
        } else if (outcome.stopped) {
            state.complete_stopped();
        } else if constexpr (std::is_void_v<T>) {
            state.complete();
        } else {
            state.complete(std::move(*outcome.value));
        }
    }

    Executor* executor_;
    const PromiseBase* owner_;
    StopToken linked_;
    ScopeMode mode_;
    JoinHandle join_;
    std::exception_ptr error_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> child_stopped_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> spawned_{false};
    bool joined_{false};
};

namespace detail {
template <class T>
struct ScopeResult {
    ScopeResult() = default;
    ScopeResult(const ScopeResult&) = delete;
    ScopeResult& operator=(const ScopeResult&) = delete;
    // A value a failed join kept from the caller goes with the frame.
    ~ScopeResult() { dstd::discard_unread(value); }

    dstd::Optional<T> value;
    void set(T v) { value.emplace(std::move(v)); }
    T take() { return std::move(*value); }
};
template <>
struct ScopeResult<void> {};

// Calls `fn` in a frame of its own, so an exception it throws before it returns
// its task reaches the caller as an outcome, and the caller still joins.
template <class R, class Fn>
CoroTask<R> call_scope_body(Fn& fn, Scope& scope) {
    if constexpr (std::is_void_v<R>) {
        co_await fn(scope);
    } else {
        co_return co_await fn(scope);
    }
}
}  // namespace detail

/**
 * @brief Runs `fn(scope)` with a new @ref Scope on the current executor and
 * always joins it, also when `fn` raises.
 *
 * An error of `fn` requests stop before the join and is raised after it. A
 * child error is raised when `fn` had none. When `fn` stops, or a child
 * stopped while the scope had no stop requested, the task of `with_scope` stops
 * after the join.
 */
template <class Fn,
          class R = typename std::invoke_result_t<Fn&, Scope&>::value_type>
CoroTask<R> with_scope(Fn fn, ScopeMode mode = ScopeMode::FailFast) {
    const PromiseBase* owner = co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    Scope scope(executor, owner, mode);
    detail::ScopeResult<R> result;
    auto body = co_await settle(detail::call_scope_body<R>(fn, scope));
    if constexpr (!std::is_void_v<R>) {
        if (body.value) result.set(std::move(*body.value));
    }
    if (body.exception || body.stopped) scope.request_stop();
    co_await scope.join_quietly();
    if (body.exception) common::rethrow_and_clear(body.exception);
    if (std::exception_ptr join_error = scope.take_error()) {
        common::rethrow_and_clear(join_error);
    }
    if (body.stopped || scope.child_stopped()) co_await stop_here();
    if constexpr (!std::is_void_v<R>) co_return result.take();
}

/// Runs @p task and ends it as cancelled at its next wait when @p token stops.
/// Anything it awaits or spawns in a @ref with_scope stops with it. When the
/// token stops the task, this task stops too.
template <class T>
CoroTask<T> with_stop_token(CoroTask<T> task, StopToken token) {
    const PromiseBase* owner = co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    Scope scope(executor, owner);
    scope.link_stop(std::move(token));
    SpawnFuture<T> future = scope.spawn_future(std::move(task));
    detail::ScopeResult<T> result;
    auto settled = co_await settle_in_place(future);
    if constexpr (!std::is_void_v<T>) {
        if (settled.value) result.set(std::move(*settled.value));
    }
    if (settled.exception || settled.stopped) future.cancel();
    co_await scope.join_quietly();
    if (settled.exception) common::rethrow_and_clear(settled.exception);
    if (settled.stopped) co_await stop_here();
    if constexpr (!std::is_void_v<T>) co_return result.take();
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_SCOPE_H
