#ifndef DFTRACER_UTILS_RUNTIME_CORO_TASK_H
#define DFTRACER_UTILS_RUNTIME_CORO_TASK_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/object_pool.h>
#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/monitor.h>
#include <dftracer/utils/runtime/coro/pool_resume.h>
#include <dftracer/utils/runtime/coro/yield.h>

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
class Executor;
}  // namespace dftracer::utils

namespace dftracer::utils::coro {

struct PromiseBase {
    std::atomic<bool> awaiting_async_{false};
    /// Set (release) when the coroutine reaches its final suspend, so a thread
    /// blocking on it sees its result through the matching acquire.
    std::atomic<bool> finished_{false};
    /// Set by the first of the awaiter (once the child returned to it) and
    /// the child's final suspend; the second one continues the awaiter.
    std::atomic<bool> handoff_{false};
    std::coroutine_handle<> continuation_{nullptr};
    TaskIndex awaited_task_id_{-1};
    Scheduler* scheduler_{nullptr};
    Executor* executor_{nullptr};
    std::atomic<bool>* cancellation_token_{nullptr};
    /// Set by CoroTask::cancel(). The next wait of this task, or of any task
    /// it awaits, ends the task as cancelled instead of waiting.
    std::atomic<bool> cancel_requested_{false};
    /// The task ended at a wait because it was cancelled, not by returning or
    /// throwing. Written before the awaiter is released, read after.
    bool cancelled_{false};
    /// Set when CoroTask::await_suspend or CoroTask::resume starts the task.
    bool started_{false};
    /// The frame holds a CoroScope, so it must reach its join: a cancelled
    /// child does not end it (see propagate_cancel).
    bool owns_scope_{false};
    /// Ends the awaiter of this task on its done path when this task ended
    /// cancelled; returns false when the awaiter must resume instead. Set by
    /// CoroTask::await_suspend for an awaiter that has propagate_cancel().
    bool (*parent_cancel_)(std::coroutine_handle<>) noexcept {nullptr};
    PromiseBase* root_promise_{nullptr};
    /// The pool current when the coroutine was created; it enters it each
    /// time it resumes.
    memory::PoolTag pool_{memory::current_pool_tag()};
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
    void* trace_handle_{nullptr};  ///< logging::coro_trace enter/leave handle
    const char* trace_file_{nullptr};  ///< coroutine definition site
    std::uint_least32_t trace_line_{0};
#endif

    static void* operator new(std::size_t size) {
        return ObjectPool::instance().allocate(size);
    }
    static void operator delete(void* ptr, std::size_t size) {
        ObjectPool::instance().deallocate(ptr, size);
    }

    void set_scheduler(Scheduler* s) { scheduler_ = s; }
    void set_executor(Executor* e) { executor_ = e; }
    Executor* get_executor() const { return executor_; }
    void set_root_promise(PromiseBase* p) { root_promise_ = p; }
    PromiseBase* get_root_promise() {
        return root_promise_ ? root_promise_ : this;
    }

    /// True once this task or the root of the chain awaiting it was cancelled.
    bool cancel_requested() const noexcept {
        return cancel_requested_.load(std::memory_order_acquire) ||
               (root_promise_ && root_promise_->cancel_requested_.load(
                                     std::memory_order_acquire));
    }

    /// Releases the awaiter of a task that ended, by return or by a throw.
    /// Nothing reads the frame after the release or the exchange: a blocked
    /// get() or the awaiter may destroy it from then on.
    void complete() noexcept {
        std::coroutine_handle<> next = continuation_;
        const bool cancelled = cancelled_;
        auto parent_cancel = parent_cancel_;
        finished_.store(true, std::memory_order_release);
        if (next && handoff_.exchange(true, std::memory_order_acq_rel)) {
            if (cancelled && parent_cancel != nullptr && parent_cancel(next))
                return;
            next.resume();
        }
    }

    /// A child of this task ended cancelled. When this task is cancelled too
    /// and holds no scope, it ends as cancelled as well, with no exception,
    /// and returns true. Call it only while this task is suspended on that
    /// child.
    bool propagate_cancel() noexcept {
        if (owns_scope_ || !cancel_requested()) return false;
        finish_cancelled();
        return true;
    }

    /// Ends the task as cancelled at the await it is suspending on, with no
    /// exception: the body is not resumed, and the owner of the frame destroys
    /// it, which runs the destructors of its locals. Call it only from the
    /// await_suspend of this task's own frame.
    void finish_cancelled() noexcept {
        cancelled_ = true;
        complete();
    }
};

namespace detail {

/// Awaited first in a coroutine that holds a CoroScope; see
/// PromiseBase::owns_scope_.
struct OwnsScopeTag {};
inline constexpr OwnsScopeTag OWNS_SCOPE{};

/// Result storage for the promise. Non-void stores the value and provides
/// return_value; void provides return_void instead.
template <typename T>
struct ResultHolder {
    T result_;
    void return_value(T value) { result_ = std::move(value); }
};

template <>
struct ResultHolder<void> {
    void return_void() noexcept {}
};

/// Lazy invoke-result so std::invoke_result_t<Func, void> is never formed.
template <typename F, typename U>
struct invoke_res {
    using type = std::invoke_result_t<F, U>;
};

template <typename F>
struct invoke_res<F, void> {
    using type = std::invoke_result_t<F>;
};

/// Result element type for operator& (AND): tuple of both for non-void left,
/// just the right's type when the left is void.
template <typename A, typename B>
struct and_value {
    using type = std::tuple<A, B>;
};

template <typename B>
struct and_value<void, B> {
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
        std::exception_ptr exception_;

        CoroTask<T> get_return_object(
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
            std::source_location loc = std::source_location::current()
#endif
        ) {
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
            this->trace_file_ = loc.file_name();
            this->trace_line_ = loc.line();
#endif
            return CoroTask{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        detail::EnterPoolOnStart initial_suspend() noexcept {
            return {this->pool_};
        }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                h.promise().complete();
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() { exception_ = std::current_exception(); }

        /// Awaiting a task or a join is not a cancel point: the awaited task
        /// reaches its own, and a scope must always reach its join.
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

        /// Marks this frame as holding a CoroScope; see owns_scope_.
        std::suspend_never await_transform(detail::OwnsScopeTag) noexcept {
            this->owns_scope_ = true;
            return {};
        }

        /// Not double-wrapped by YieldCheckAwaitable: YieldAwaitable makes
        /// its own timeslice check.
        auto await_transform(coro::YieldAwaitable y) {
            return detail::InPool<coro::YieldAwaitable>{
                y, this->pool_, false, this->cancel_requested()};
        }

        /// Wrap every other awaitable in a timeslice check.
        /// Movable rvalue awaitables are moved into the wrapper so
        /// the temporary does not dangle across a suspension.
        /// Lvalue awaitables and non-movable rvalues stay as refs.
        template <typename U>
        auto await_transform(U&& awaitable) {
            const bool cancelled = at_cancel_point<U>();
            if constexpr (std::is_lvalue_reference_v<U>) {
                return coro::detail::YieldCheckAwaitable<U>{
                    awaitable, this->pool_, false, false, cancelled};
            } else if constexpr (std::is_move_constructible_v<U>) {
                return coro::detail::YieldCheckAwaitable<U>{
                    std::move(awaitable), this->pool_, false, false, cancelled};
            } else {
                return coro::detail::YieldCheckAwaitable<U&&>{
                    static_cast<U&&>(awaitable), this->pool_, false, false,
                    cancelled};
            }
        }
    };

   private:
    std::coroutine_handle<promise_type> coro_handle_;

   public:
    /// Awaiting a task is not a cancel point of the awaiter (see
    /// promise_type::at_cancel_point).
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
        p.continuation_ = awaiting_coro;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* awaiting_root = awaiting_coro.promise().get_root_promise();
            p.set_root_promise(awaiting_root);
        }

        // Deep mode: capture this co_await'd child (run on the awaiter's
        // stack, so it never passes through the executor queue) and make it
        // the current coroutine so its own awaits nest under it.
        if (utilities::monitor_deep_enabled()) {
            utilities::monitor_resume_begin(utilities::monitor_enqueue(
                coro_handle_.address(), utilities::CoroKind::Sync));
        }

        if (coro_handle_.done()) return false;

#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
        if (logger::detail::enabled(logger::Level::Trace)) [[unlikely]] {
            p.trace_handle_ = logger::detail::coro_trace_enter(
                coro_handle_.address(), p.trace_file_, p.trace_line_);
        }
#endif
        p.started_ = true;
        if constexpr (requires {
                          awaiting_coro.promise().propagate_cancel();
                      }) {
            p.parent_cancel_ = [](std::coroutine_handle<> h) noexcept {
                return std::coroutine_handle<Promise>::from_address(h.address())
                    .promise()
                    .propagate_cancel();
            };
        }
        coro_handle_.resume();
        if constexpr (requires {
                          awaiting_coro.promise().propagate_cancel();
                      }) {
            // The child ended cancelled before it returned here.
            if (p.cancelled_ && awaiting_coro.promise().propagate_cancel())
                return true;
        }
        return !p.handoff_.exchange(true, std::memory_order_acq_rel);
    }

    T await_resume() {
#if defined(__GNUC__) || defined(__clang__)
        // Never null here (await_resume runs on a live task); silences a
        // spurious GCC -Wnull-dereference on the promise() reads below.
        if (!coro_handle_) __builtin_unreachable();
#endif
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
        if (coro_handle_.promise().trace_handle_) [[unlikely]] {
            logger::detail::coro_trace_leave(
                coro_handle_.promise().trace_handle_);
        }
#endif
        if (utilities::monitor_deep_enabled()) {
            utilities::monitor_sync_complete(coro_handle_.address());
        }
        if (coro_handle_.promise().cancelled_) {
            throw_cancelled("task cancelled");
        }
        if (coro_handle_.promise().exception_) {
            rethrow_and_clear(coro_handle_.promise().exception_);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(coro_handle_.promise().result_);
        }
    }

    // ========================================================================
    // Manual control (non-coroutine interface)
    // ========================================================================

    /**
     * Ask the task to stop. Thread-safe and idempotent. Cooperative: at its
     * next wait, and at that of any task it awaits, the task ends as
     * cancelled without an exception, and its owner destroys the suspended
     * frame, which runs the destructors of its locals. Awaiting a task or a
     * scope join is not a wait. Call it on the task that was handed to its
     * awaiter, not on one it awaits.
     *
     * An awaiter that wants no exception tests cancelled() before
     * await_resume(); await_resume() of a cancelled task throws a cancelled
     * error (Condition::Cancelled).
     */
    void cancel() noexcept {
        if (coro_handle_) {
            coro_handle_.promise().cancel_requested_.store(
                true, std::memory_order_release);
        }
    }

    /// True when the task ended at a wait because it was cancelled.
    bool cancelled() const noexcept {
        return coro_handle_ && coro_handle_.promise().cancelled_;
    }

    /// True once the task ran or began to run. A task never awaited has not.
    bool started() const noexcept {
        return coro_handle_ && coro_handle_.promise().started_;
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
            memory::Scope scope{memory::Manager::get().general()};
            coro_handle_.promise().started_ = true;
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
        drive_to_completion(coro_handle_, coro_handle_.promise().finished_);
        return await_resume();
    }

    /**
     * Check if coroutine has pending exception
     */
    bool has_exception() const noexcept {
        return coro_handle_ && coro_handle_.promise().exception_ != nullptr;
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
     * @return New CoroTask holding the transformed result
     *
     * Usage:
     * @code
     * auto result = co_await compute_async()
     *     .then([](int x) { return x * 2; })
     *     .then([](int x) { return std::to_string(x); });
     * @endcode
     */
    template <typename Func>
    auto then(Func&& func) && -> CoroTask<
        typename detail::invoke_res<Func, T>::type> {
        // Pass self as parameter to ensure capture before lazy coroutine
        // suspends
        return
            [](CoroTask<T> self, auto f)
                -> CoroTask<typename detail::invoke_res<decltype(f), T>::type> {
                using U = typename detail::invoke_res<decltype(f), T>::type;
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
     * @return Transformed CoroTask
     *
     * Usage:
     * @code
     * auto result = co_await compute_async()
     *     > [](int x) { return x * 2; }
     *     > [](int x) { return std::to_string(x); };
     * @endcode
     */
    template <typename Func>
    auto operator>(Func&& func) && -> CoroTask<
        typename detail::invoke_res<Func, T>::type> {
        return std::move(*this).then(std::forward<Func>(func));
    }

    /**
     * Operator< for reverse composition (func receives this task's result)
     * @param func Transformation function
     * @return Transformed CoroTask
     *
     * Usage:
     * @code
     * auto result = co_await [](int x) { return std::to_string(x); }
     *     < compute_async();
     * @endcode
     */
    template <typename Func>
    friend auto operator<(Func&& func, CoroTask<T>&& task)
        -> CoroTask<typename detail::invoke_res<Func, T>::type> {
        return std::move(task).then(std::forward<Func>(func));
    }

    /**
     * Operator& for parallel composition (AND) - run both tasks, return tuple
     * @param lhs First task to run in parallel
     * @param rhs Second task to run in parallel
     * @return CoroTask holding a tuple of both results
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
        -> CoroTask<typename detail::and_value<T, U>::type> {
        return [](CoroTask<T> left, CoroTask<U> right)
                   -> CoroTask<typename detail::and_value<T, U>::type> {
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
            throw DFTUtilsException(ErrorCode::Internal,
                                    "Unreachable code in or_else reached");
        }(std::move(*this), std::move(fallback));
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_TASK_H
