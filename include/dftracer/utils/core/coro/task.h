#ifndef DFTRACER_UTILS_CORE_CORO_TASK_H
#define DFTRACER_UTILS_CORE_CORO_TASK_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/exception_helpers.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/object_pool.h>
#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/utilities/monitor.h>

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
    std::coroutine_handle<> continuation_{nullptr};
    TaskIndex awaited_task_id_{-1};
    Scheduler* scheduler_{nullptr};
    Executor* executor_{nullptr};
    std::atomic<bool>* cancellation_token_{nullptr};
    PromiseBase* root_promise_{nullptr};
#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
    void* trace_handle_{nullptr};      // logging::coro_trace enter/leave handle
    const char* trace_file_{nullptr};  // coroutine definition site
    std::uint_least32_t trace_line_{0};
#endif

    static void* operator new(std::size_t size) {
        return ObjectPool::instance().allocate(size);
    }
    static void operator delete(void* ptr, std::size_t size) {
        ObjectPool::instance().deallocate(ptr, size);
    }

    void set_awaited_task_id(TaskIndex id) { awaited_task_id_ = id; }
    TaskIndex get_awaited_task_id() const { return awaited_task_id_; }
    void set_scheduler(Scheduler* s) { scheduler_ = s; }
    Scheduler* get_scheduler() const { return scheduler_; }
    void set_executor(Executor* e) { executor_ = e; }
    Executor* get_executor() const { return executor_; }
    void set_root_promise(PromiseBase* p) { root_promise_ = p; }
    PromiseBase* get_root_promise() {
        return root_promise_ ? root_promise_ : this;
    }
};

namespace detail {

// Result storage for the promise. Non-void stores the value and provides
// return_value; void provides return_void instead.
template <typename T>
struct ResultHolder {
    T result_;
    void return_value(T value) { result_ = std::move(value); }
};

template <>
struct ResultHolder<void> {
    void return_void() noexcept {}
};

// Lazy invoke-result so std::invoke_result_t<Func, void> is never formed.
template <typename F, typename U>
struct invoke_res {
    using type = std::invoke_result_t<F, U>;
};

template <typename F>
struct invoke_res<F, void> {
    using type = std::invoke_result_t<F>;
};

// Result element type for operator& (AND): tuple of both for non-void left,
// just the right's type when the left is void.
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

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation_) {
                    return h.promise().continuation_;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() { exception_ = std::current_exception(); }

        // Pass YieldAwaitable through unmodified so it is not
        // double-wrapped by YieldCheckAwaitable.
        coro::YieldAwaitable await_transform(coro::YieldAwaitable y) noexcept {
            return y;
        }

        // Wrap every other awaitable in a timeslice check.
        // Movable rvalue awaitables are moved into the wrapper so
        // the temporary does not dangle across a suspension.
        // Lvalue awaitables and non-movable rvalues stay as refs.
        template <typename U>
        auto await_transform(U&& awaitable) noexcept {
            if constexpr (std::is_lvalue_reference_v<U>) {
                return coro::detail::YieldCheckAwaitable<U>{awaitable};
            } else if constexpr (std::is_move_constructible_v<U>) {
                return coro::detail::YieldCheckAwaitable<U>{
                    std::move(awaitable)};
            } else {
                return coro::detail::YieldCheckAwaitable<U&&>{
                    static_cast<U&&>(awaitable)};
            }
        }
    };

   private:
    std::coroutine_handle<promise_type> coro_handle_;

   public:
    // Type alias for result type (used by combinators)
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

    // Move-only semantics (coroutine handle is unique)
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

    template <typename Promise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> awaiting_coro) noexcept {
        coro_handle_.promise().continuation_ = awaiting_coro;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* awaiting_root = awaiting_coro.promise().get_root_promise();
            coro_handle_.promise().set_root_promise(awaiting_root);
        }

        // Deep mode: capture this co_await'd child (reached by symmetric
        // transfer, so it never passes through the executor queue) and make it
        // the current coroutine so its own awaits nest under it.
        if (utilities::monitor_deep_enabled()) {
            utilities::monitor_resume_begin(utilities::monitor_enqueue(
                coro_handle_.address(), utilities::CoroKind::Sync));
        }

        if (coro_handle_.done()) {
            return awaiting_coro;
        }

#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
        if (logger::detail::enabled(logger::Level::Trace)) [[unlikely]] {
            auto& p = coro_handle_.promise();
            p.trace_handle_ = logger::detail::coro_trace_enter(
                coro_handle_.address(), p.trace_file_, p.trace_line_);
        }
#endif
        return coro_handle_;
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
     * Check if coroutine has completed
     */
    bool done() const noexcept { return coro_handle_ && coro_handle_.done(); }

    /**
     * Resume coroutine execution (manual control)
     * Only resume if not already done
     */
    void resume() {
        if (coro_handle_ && !coro_handle_.done()) {
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
        SyncScope sync;
        while (coro_handle_ && !coro_handle_.done()) {
            coro_handle_.resume();
        }
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

    /**
     * Check if coroutine is suspended for async work
     * If true, executor should NOT drive it synchronously
     */
    bool is_awaiting_async() const noexcept {
        return coro_handle_ && coro_handle_.promise().awaiting_async_.load(
                                   std::memory_order_acquire);
    }

    /**
     * Set/clear async await flag (used by TaskFuture)
     */
    void set_awaiting_async(bool value) noexcept {
        if (coro_handle_) {
            coro_handle_.promise().awaiting_async_.store(
                value, std::memory_order_release);
        }
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
     * Operator| for OR/fallback composition - try first, fall back to second
     * @param fallback Fallback task to run if this task fails
     * @return CoroTask<T> with result from whichever succeeds
     *
     * Usage:
     * @code
     * auto result = co_await (primary_task() | fallback_task());
     * @endcode
     */
    friend auto operator|(CoroTask<T>&& primary, CoroTask<T>&& fallback)
        -> CoroTask<T> {
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
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "Unreachable code in operator| reached");
        }(std::move(primary), std::move(fallback));
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_TASK_H
