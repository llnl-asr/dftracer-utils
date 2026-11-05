// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_TASK_H
#define DFTRACER_UTILS_CORO_TASK_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/detail/monitor.h>
#include <dftracer/utils/coro/detail/pool_resume.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/either.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <coroutine>
#include <exception>
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
#include <cstdint>
#include <source_location>
#endif
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace dftracer::utils {
class Scheduler;
}  // namespace dftracer::utils

namespace dftracer::utils::coro {

class Executor;
class Scope;

template <typename T>
class CoroTask;

namespace detail {
template <typename L, typename R>
CoroTask<dstd::Either<L, R>> either_task(
    dstd::Either<CoroTask<L>, CoroTask<R>> pair);
}  // namespace detail

namespace detail {
/// True when a task that finished may resume the awaiter on this thread,
/// because the thread runs the awaiter's executor, runs no executor, or the
/// awaiter has none.
bool runs_on(Executor* home) noexcept;
/// Queues `awaiter` on `home`, the executor it runs on.
void resume_on_home(Executor* home, std::coroutine_handle<> awaiter) noexcept;
/// Executor::current(), for code that sees only a declaration of Executor.
Executor* current_executor() noexcept;
/// Executor::wake_cancelled, for code that sees only a declaration of Executor.
void wake_cancelled_on(Executor* executor) noexcept;
}  // namespace detail

namespace this_coro {
struct ExecutorTag {};
/// `co_await this_coro::executor` gives the executor the task runs on.
inline constexpr ExecutorTag executor{};
struct StopRequestedTag {};
/// `co_await this_coro::stop_requested` is true when the next wait of this task
/// would end it as cancelled, for a loop that computes without waits.
inline constexpr StopRequestedTag stop_requested{};
}  // namespace this_coro

namespace detail {
struct ExecutorQuery {
    Executor* value;
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    Executor* await_resume() const noexcept { return value; }
};
}  // namespace detail

class Scope;
struct PromiseBase;

namespace detail {
/// True when the scope was asked to stop or its owning task is cancelled.
bool scope_stopping(const Scope& scope) noexcept;

template <typename T>
struct ValueQuery {
    T value;
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    T await_resume() const noexcept { return value; }
};
}  // namespace detail

struct PromiseBase : detail::FrameAllocation {
    std::atomic<bool> awaiting_async{false};
    /// Set (release) when the coroutine reaches its final suspend, so a thread
    /// blocking on it sees its result through the matching acquire.
    std::atomic<bool> finished{false};
    /// Set by the first of the awaiter (once the child returned to it) and
    /// the child's final suspend; the second one continues the awaiter.
    std::atomic<bool> handoff{false};
    std::coroutine_handle<> continuation{nullptr};
    common::TaskIndex awaited_task_id{-1};
    Scheduler* scheduler{nullptr};
    Executor* executor{nullptr};
    /// The executor the awaiter runs on. A task that ends elsewhere posts the
    /// awaiter back to it instead of resuming it on this thread.
    Executor* continuation_executor{nullptr};
    /// The @ref Scope that spawned this task, so the task cannot join it.
    const Scope* scope{nullptr};
    /// A flag outside the frame that cancels this task and those it awaits,
    /// set through the future of a @ref Scope spawn.
    const std::atomic<bool>* cancel_source{nullptr};
    std::atomic<bool> cancel_flag{false};
    /// Written before the awaiter is released and read after.
    bool cancelled{false};
    bool started{false};
    /// A cancelled child does not end a frame that holds a scope, which must
    /// reach its join.
    bool owns_scope{false};
    /// Ends the awaiter on this task's done path when it ended cancelled;
    /// false means the awaiter resumes instead.
    bool (*parent_cancel)(std::coroutine_handle<>) noexcept {nullptr};
    PromiseBase* root_promise{nullptr};
    detail::RunContext context;
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
    void* trace_handle{nullptr};  ///< logging::coro_trace enter/leave handle
    const char* trace_file{nullptr};  ///< coroutine definition site
    std::uint_least32_t trace_line{0};
#endif

    void set_scheduler(Scheduler* s) { scheduler = s; }
    void set_executor(Executor* e) { executor = e; }
    Executor* get_executor() const { return executor; }
    void set_root_promise(PromiseBase* p) { root_promise = p; }
    PromiseBase* get_root_promise() {
        return root_promise ? root_promise : this;
    }

    bool cancel_requested() const noexcept {
        const PromiseBase* root = root_promise ? root_promise : this;
        return cancel_flag.load(std::memory_order_acquire) ||
               root->cancel_flag.load(std::memory_order_acquire) ||
               (root->cancel_source &&
                root->cancel_source->load(std::memory_order_acquire)) ||
               (root->scope && detail::scope_stopping(*root->scope));
    }

    /// Releases the awaiter of a task that ended. The frame must not be read
    /// afterwards, since the awaiter may destroy it.
    void complete() noexcept {
        std::coroutine_handle<> next = continuation;
        Executor* const home = continuation_executor;
        const bool was_cancelled = cancelled;
        auto parent_cancel_fn = parent_cancel;
        finished.store(true, std::memory_order_release);
        if (!next) {
            notify_task_finished();
            return;
        }
        if (handoff.exchange(true, std::memory_order_acq_rel)) {
            if (was_cancelled && parent_cancel_fn != nullptr &&
                parent_cancel_fn(next))
                return;
            if (detail::runs_on(home)) {
                next.resume();
            } else {
                detail::resume_on_home(home, next);
            }
        }
    }

    /// Ends this task as cancelled too, with no exception, when it holds no
    /// scope and is cancelled, and returns true then. Call only while suspended
    /// on that child.
    bool propagate_cancel() noexcept {
        if (owns_scope || !cancel_requested()) return false;
        finish_cancelled();
        return true;
    }

    /// Ends the task as cancelled with no exception. The owner destroys the
    /// frame. Call only from the await_suspend of this task's own frame.
    void finish_cancelled() noexcept {
        cancelled = true;
        complete();
    }
};

namespace detail {

struct OwnsScopeTag {};
inline constexpr OwnsScopeTag OWNS_SCOPE{};

/// Result storage for the promise. Non-void stores the value and provides
/// return_value; void provides return_void instead.
template <typename T>
struct ResultHolder {
    T result;
    void return_value(T value) { result = std::move(value); }
    T take_result() { return std::move(result); }
};

/// A type with no default state, such as an Either or a generator, waits in an
/// Optional.
template <typename T>
    requires(!std::is_void_v<T> && !std::is_default_constructible_v<T>)
struct ResultHolder<T> {
    dstd::Optional<T> result;
    void return_value(T value) { result.emplace(std::move(value)); }
    T take_result() { return std::move(*result); }
};

template <>
struct ResultHolder<void> {
    void return_void() noexcept {}
};

/// Lazy invoke-result so std::invoke_result_t<Func, void> is never formed.
template <typename F, typename U>
struct InvokeRes {
    using type = std::invoke_result_t<F, U>;
};

template <typename F>
struct InvokeRes<F, void> {
    using type = std::invoke_result_t<F>;
};

/// Result element type for operator& (AND): tuple of both for non-void left,
/// just the right's type when the left is void.
template <typename A, typename B>
struct AndValue {
    using type = dstd::Tuple<A, B>;
};

template <typename B>
struct AndValue<void, B> {
    using type = B;
};

}  // namespace detail

/**
 * CoroTask<T>
 *
 * User-facing coroutine task type
 *
 * Usage:
 * @code
 * CoroTask<int> compute_async() {
 *     auto data = co_await read_file();
 *     co_return process(data);
 * }
 *
 * // In another coroutine:
 * int result = co_await compute_async();
 * @endcode
 */
template <typename T = void>
class CoroTask {
   public:
    struct promise_type : PromiseBase, detail::ResultHolder<T> {
        std::exception_ptr exception;

        CoroTask<T> get_return_object(
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
            std::source_location loc = std::source_location::current()
#endif
        ) {
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
            this->trace_file = loc.file_name();
            this->trace_line = loc.line();
#endif
            return CoroTask{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        detail::EnterPoolOnStart initial_suspend() noexcept {
            return {&this->context};
        }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                h.promise().complete();
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() { exception = std::current_exception(); }

        /// Awaiting a task or a join is not a cancel point. A scope must always
        /// reach its join.
        template <typename U>
        static constexpr bool CANCEL_EXEMPT_V =
            requires { requires std::remove_cvref_t<U>::CANCEL_EXEMPT; };

        template <typename U>
        bool at_cancel_point() const noexcept {
            if constexpr (CANCEL_EXEMPT_V<U>) {
                return false;
            } else {
                return this->cancel_requested();
            }
        }

        /// Gives the promise, so a @ref Scope in this frame can name its owner.
        detail::ValueQuery<const PromiseBase*> await_transform(
            detail::OwnsScopeTag) noexcept {
            this->owns_scope = true;
            return {this};
        }

        detail::ValueQuery<bool> await_transform(
            this_coro::StopRequestedTag) noexcept {
            return {this->cancel_requested()};
        }

        detail::ExecutorQuery await_transform(this_coro::ExecutorTag) noexcept {
            return {this->executor != nullptr ? this->executor
                                              : detail::current_executor()};
        }

        /// `co_await` on one of two tasks gives the value of the one held, on
        /// the same side.
        template <typename L, typename R>
        auto await_transform(dstd::Either<CoroTask<L>, CoroTask<R>>&& pair) {
            static_assert(!std::is_void_v<L> && !std::is_void_v<R>,
                          "an Either of tasks needs a value from each side");
            return await_transform(detail::either_task(std::move(pair)));
        }

        /// @ref YieldAwaitable makes its own timeslice check.
        auto await_transform(coro::YieldAwaitable y) {
            return detail::InPool<coro::YieldAwaitable>{
                y, &this->context, false, this->cancel_requested()};
        }

        /// Wrap every other awaitable in a timeslice check.
        /// Movable rvalue awaitables are moved into the wrapper so
        /// the temporary does not dangle across a suspension.
        /// Lvalue awaitables and non-movable rvalues stay as refs.
        template <typename U>
        auto await_transform(U&& awaitable) {
            const bool at_cancel = at_cancel_point<U>();
            if constexpr (std::is_lvalue_reference_v<U>) {
                return coro::detail::YieldCheckAwaitable<U>{
                    awaitable, &this->context, false, false, at_cancel};
            } else if constexpr (std::is_move_constructible_v<U>) {
                return coro::detail::YieldCheckAwaitable<U>{
                    std::move(awaitable), &this->context, false, false,
                    at_cancel};
            } else {
                return coro::detail::YieldCheckAwaitable<U&&>{
                    static_cast<U&&>(awaitable), &this->context, false, false,
                    at_cancel};
            }
        }
    };

   private:
    std::coroutine_handle<promise_type> coro_handle_;

   public:
    static constexpr bool CANCEL_EXEMPT = true;

    /// Type alias for result type (used by combinators)
    using value_type = T;
    using result_type = T;

    /**
     * Constructor from coroutine handle
     * Called by promise_type::get_return_object()
     */
    explicit CoroTask(std::coroutine_handle<promise_type> h)
        : coro_handle_(h) {}

    /**
     * Destructor - clean up coroutine state
     */
    ~CoroTask() {
        if (coro_handle_) {
            auto h = coro_handle_;
            coro_handle_ = nullptr;
            h.destroy();
        }
    }

    /// Move-only semantics (coroutine handle is unique)
    CoroTask(const CoroTask&) = delete;
    CoroTask& operator=(const CoroTask&) = delete;

    CoroTask(CoroTask&& other) noexcept : coro_handle_(other.coro_handle_) {
        other.coro_handle_ = nullptr;
    }

    CoroTask& operator=(CoroTask&& other) noexcept {
        if (this != &other) {
            if (coro_handle_) {
                coro_handle_.destroy();
            }
            coro_handle_ = other.coro_handle_;
            other.coro_handle_ = nullptr;
        }
        return *this;
    }

    // ========================================================================
    // Awaitable interface - enables co_await
    // ========================================================================

    /**
     * Check if coroutine already completed
     * If true, no suspension needed (optimization)
     */
    bool await_ready() const noexcept { return coro_handle_.done(); }

    // The child runs on this stack, not by symmetric transfer: GCC makes that
    // transfer a tail call only with sibling-call optimization, and without
    // it each child that ends at once would add frames until the stack
    // overflows. A child that suspends continues the awaiter itself.
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiting_coro) noexcept {
        auto& p = coro_handle_.promise();
        p.continuation = awaiting_coro;

        Executor* awaiter_executor = nullptr;
        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* awaiting_root = awaiting_coro.promise().get_root_promise();
            p.set_root_promise(awaiting_root);
            awaiter_executor = awaiting_coro.promise().get_executor();
        }
        if (awaiter_executor == nullptr)
            awaiter_executor = detail::current_executor();
        p.continuation_executor = awaiter_executor;
        if (p.executor == nullptr) p.executor = awaiter_executor;

        // Deep mode: capture this co_await'd child (run on the awaiter's
        // stack, so it never passes through the executor queue) and make it
        // the current coroutine so its own awaits nest under it.
        if (detail::monitor_deep_enabled()) {
            detail::monitor_resume_begin(detail::monitor_enqueue(
                coro_handle_.address(), detail::CoroKind::Sync));
        }

        if (coro_handle_.done()) return false;

#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
        if (common::logger::detail::enabled(common::logger::Level::Trace))
            [[unlikely]] {
            p.trace_handle = common::logger::detail::coro_trace_enter(
                coro_handle_.address(), p.trace_file, p.trace_line);
        }
#endif
        p.started = true;
        if constexpr (requires {
                          awaiting_coro.promise().propagate_cancel();
                      }) {
            p.parent_cancel = [](std::coroutine_handle<> h) noexcept {
                return std::coroutine_handle<Promise>::from_address(h.address())
                    .promise()
                    .propagate_cancel();
            };
        }
        coro_handle_.resume();
        if constexpr (requires {
                          awaiting_coro.promise().propagate_cancel();
                      }) {
            if (p.cancelled && awaiting_coro.promise().propagate_cancel())
                return true;
        }
        return !p.handoff.exchange(true, std::memory_order_acq_rel);
    }

    T await_resume() {
#if defined(__GNUC__) || defined(__clang__)
        // Never null here (await_resume runs on a live task); silences a
        // spurious GCC -Wnull-dereference on the promise() reads below.
        if (!coro_handle_) __builtin_unreachable();
#endif
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
        if (coro_handle_.promise().trace_handle) [[unlikely]] {
            common::logger::detail::coro_trace_leave(
                coro_handle_.promise().trace_handle);
        }
#endif
        if (detail::monitor_deep_enabled()) {
            detail::monitor_sync_complete(coro_handle_.address());
        }
        if (coro_handle_.promise().cancelled) {
            throw_cancelled("task cancelled");
        }
        if (coro_handle_.promise().exception) {
            common::rethrow_and_clear(coro_handle_.promise().exception);
        }
        if constexpr (!std::is_void_v<T>) {
            return coro_handle_.promise().take_result();
        }
    }

    // ========================================================================
    // Manual control (non-coroutine interface)
    // ========================================================================

    /**
     * Asks the task to stop. Thread-safe and idempotent. At its next wait, or
     * that of a task it awaits, it ends as cancelled. Call it on the task
     * handed to its awaiter, not one it awaits. await_resume() throws
     * dstd::Condition::Cancelled, so test cancelled() first to avoid it.
     */
    void cancel() noexcept {
        if (coro_handle_) {
            auto& promise = coro_handle_.promise();
            promise.cancel_flag.store(true, std::memory_order_release);
            if (promise.executor) detail::wake_cancelled_on(promise.executor);
        }
    }

    bool cancelled() const noexcept {
        return coro_handle_ && coro_handle_.promise().cancelled;
    }

    bool started() const noexcept {
        return coro_handle_ && coro_handle_.promise().started;
    }

    /**
     * Check if coroutine has completed
     */
    bool done() const noexcept { return coro_handle_ && coro_handle_.done(); }

    /**
     * Resume coroutine execution (manual control)
     * Only resume if not already done
     */
    void resume() {
        if (coro_handle_ && !coro_handle_.done()) {
            memory::detail::Scope scope{
                memory::detail::Manager::get().general()};
            coro_handle_.promise().started = true;
            coro_handle_.resume();
        }
    }

    /**
     * Get result (blocking, for non-coroutine callers)
     * Resumes coroutine until completion
     *
     * @return The result value
     * @throws Exception if coroutine threw
     */
    T get() {
        drive_to_completion(coro_handle_, coro_handle_.promise().finished);
        return await_resume();
    }

    /**
     * Check if coroutine has pending exception
     */
    bool has_exception() const noexcept {
        return coro_handle_ && coro_handle_.promise().exception != nullptr;
    }

    /**
     * Get coroutine handle (for advanced use)
     */
    std::coroutine_handle<promise_type> handle() const noexcept {
        return coro_handle_;
    }

    // ========================================================================
    // Combinators and syntactic sugar
    // ========================================================================

    /**
     * Chain operation using then() - transform result with a function
     * @param func Transformation function (T -> U)
     * @return New @ref CoroTask holding the transformed result
     *
     * Usage:
     * @code
     * auto result = co_await compute_async()
     *     .then([](int x) { return x * 2; })
     *     .then([](int x) { return dstd::to_string(x); });
     * @endcode
     */
    template <typename Func>
    auto then(
        Func&& func) && -> CoroTask<typename detail::InvokeRes<Func, T>::type> {
        // Pass self as parameter to ensure capture before lazy coroutine
        // suspends
        return
            [](CoroTask<T> self, auto f)
                -> CoroTask<typename detail::InvokeRes<decltype(f), T>::type> {
                using U = typename detail::InvokeRes<decltype(f), T>::type;
                if constexpr (std::is_void_v<T>) {
                    co_await std::move(self);
                    if constexpr (std::is_void_v<U>) {
                        f();
                        co_return;
                    } else {
                        co_return f();
                    }
                } else {
                    T result = co_await std::move(self);
                    if constexpr (std::is_void_v<U>) {
                        f(std::move(result));
                        co_return;
                    } else {
                        co_return f(std::move(result));
                    }
                }
            }(std::move(*this), std::forward<Func>(func));
    }

    /**
     * Tap operation - inspect value without transforming it
     * @param func Inspection function (T -> void)
     * @return CoroTask<T> with same value
     *
     * Usage:
     * @code
     * auto result = co_await compute_async()
     *     .tap([](int x) { std::cout << "Got: " << x << "\n"; })
     *     .then([](int x) { return x * 2; });
     * @endcode
     */
    template <typename Func>
    auto tap(Func&& func) && -> CoroTask<T> {
        return [](CoroTask<T> self, auto f) -> CoroTask<T> {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(self);
                f();
                co_return;
            } else {
                T result = co_await std::move(self);
                f(result);
                co_return result;
            }
        }(std::move(*this), std::forward<Func>(func));
    }

    /**
     * Operator> for chaining (same as then())
     * @param func Transformation function
     * @return Transformed @ref CoroTask
     *
     * Usage:
     * @code
     * auto result = co_await compute_async()
     *     > [](int x) { return x * 2; }
     *     > [](int x) { return dstd::to_string(x); };
     * @endcode
     */
    template <typename Func>
    auto operator>(
        Func&& func) && -> CoroTask<typename detail::InvokeRes<Func, T>::type> {
        return std::move(*this).then(std::forward<Func>(func));
    }

    /**
     * Operator< for reverse composition (func receives this task's result)
     * @param func Transformation function
     * @return Transformed @ref CoroTask
     *
     * Usage:
     * @code
     * auto result = co_await [](int x) { return dstd::to_string(x); }
     *     < compute_async();
     * @endcode
     */
    template <typename Func>
    friend auto operator<(Func&& func, CoroTask<T>&& task)
        -> CoroTask<typename detail::InvokeRes<Func, T>::type> {
        return std::move(task).then(std::forward<Func>(func));
    }

    /**
     * Operator& for parallel composition (AND) - run both tasks, return tuple
     * @param lhs First task to run in parallel
     * @param rhs Second task to run in parallel
     * @return @ref CoroTask holding a tuple of both results
     *
     * Note: In the current synchronous execution model, these run sequentially.
     * For true parallel execution, use CoroScope::spawn().
     *
     * Usage:
     * @code
     * auto [result1, result2] = co_await (task1() & task2());
     * @endcode
     */
    template <typename U>
    friend auto operator&(CoroTask<T>&& lhs, CoroTask<U>&& rhs)
        -> CoroTask<typename detail::AndValue<T, U>::type> {
        return [](CoroTask<T> left, CoroTask<U> right)
                   -> CoroTask<typename detail::AndValue<T, U>::type> {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(left);
                co_return co_await std::move(right);
            } else {
                T result1 = co_await std::move(left);
                U result2 = co_await std::move(right);
                co_return std::make_tuple(std::move(result1),
                                          std::move(result2));
            }
        }(std::move(lhs), std::move(rhs));
    }

    /**
     * or_else - OR/fallback composition: run this task, and if it throws, run
     * the fallback instead. Yields whichever succeeds.
     *
     * @param fallback Fallback task to run if this task fails
     * @return CoroTask<T> with result from whichever succeeds
     *
     * Usage:
     * @code
     * auto result = co_await primary_task().or_else(fallback_task());
     * @endcode
     */
    auto or_else(CoroTask<T>&& fallback) && -> CoroTask<T> {
        return [](CoroTask<T> prim, CoroTask<T> fall) -> CoroTask<T> {
            std::exception_ptr primary_exception;
            try {
                if constexpr (std::is_void_v<T>) {
                    co_await std::move(prim);
                    co_return;
                } else {
                    co_return co_await std::move(prim);
                }
            } catch (...) {
                primary_exception = std::current_exception();
            }
            if (primary_exception) {
                if constexpr (std::is_void_v<T>) {
                    co_await std::move(fall);
                    co_return;
                } else {
                    co_return co_await std::move(fall);
                }
            }
            throw common::DFTUtilsException(
                common::ErrorCode::Internal,
                "Unreachable code in or_else reached");
        }(std::move(*this), std::move(fallback));
    }
};

namespace detail {
template <typename L, typename R>
CoroTask<dstd::Either<L, R>> either_task(
    dstd::Either<CoroTask<L>, CoroTask<R>> pair) {
    if (pair.is_left()) {
        L value = co_await std::move(pair.left());
        co_return dstd::Either<L, R>::left(std::move(value));
    }
    R value = co_await std::move(pair.right());
    co_return dstd::Either<L, R>::right(std::move(value));
}
}  // namespace detail

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_TASK_H
