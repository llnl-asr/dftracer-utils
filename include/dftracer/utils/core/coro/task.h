#ifndef DFTRACER_UTILS_CORE_CORO_TASK_H
#define DFTRACER_UTILS_CORE_CORO_TASK_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/coro/yield.h>

#include <atomic>
#include <coroutine>
#include <exception>
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
    struct promise_type : PromiseBase {
        T result_;
        std::exception_ptr exception_;

        CoroTask<T> get_return_object() {
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

        void return_value(T value) { result_ = std::move(value); }

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
            coro_handle_.destroy();
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

        if (coro_handle_.done()) {
            return awaiting_coro;
        }

        return coro_handle_;
    }

    T await_resume() {
        if (coro_handle_.promise().exception_) {
            std::rethrow_exception(coro_handle_.promise().exception_);
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
     * @return New CoroTask<U> with transformed result
     *
     * Usage:
     * @code
     * auto result = co_await compute_async()
     *     .then([](int x) { return x * 2; })
     *     .then([](int x) { return std::to_string(x); });
     * @endcode
     */
    template <typename Func>
    auto then(Func&& func) && -> CoroTask<std::invoke_result_t<Func, T>> {
        // Pass self as parameter to ensure capture before lazy coroutine
        // suspends
        return [](CoroTask<T> self,
                  auto f) -> CoroTask<std::invoke_result_t<decltype(f), T>> {
            using U = std::invoke_result_t<decltype(f), T>;
            T result = co_await std::move(self);
            if constexpr (std::is_void_v<U>) {
                f(std::move(result));
                co_return;
            } else {
                co_return f(std::move(result));
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
            T result = co_await std::move(self);
            f(result);
            co_return result;
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
    auto operator>(Func&& func) && -> CoroTask<std::invoke_result_t<Func, T>> {
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
        -> CoroTask<std::invoke_result_t<Func, T>> {
        return [](CoroTask<T> self,
                  auto f) -> CoroTask<std::invoke_result_t<decltype(f), T>> {
            using U = std::invoke_result_t<decltype(f), T>;
            T result = co_await std::move(self);
            if constexpr (std::is_void_v<U>) {
                f(std::move(result));
                co_return;
            } else {
                co_return f(std::move(result));
            }
        }(std::move(task), std::forward<Func>(func));
    }

    /**
     * Operator& for parallel composition (AND) - run both tasks, return tuple
     * @param other Second task to run in parallel
     * @return CoroTask<std::tuple<T, U>> with both results
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
        -> CoroTask<std::tuple<T, U>> {
        return [](CoroTask<T> left,
                  CoroTask<U> right) -> CoroTask<std::tuple<T, U>> {
            T result1 = co_await std::move(left);
            U result2 = co_await std::move(right);
            co_return std::make_tuple(std::move(result1), std::move(result2));
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
                co_return co_await std::move(prim);
            } catch (...) {
                primary_exception = std::current_exception();
            }
            if (primary_exception) {
                co_return co_await std::move(fall);
            }
            throw std::logic_error("Unreachable code in operator| reached");
        }(std::move(primary), std::move(fallback));
    }
};

/**
 * Specialization for void return type
 * Simpler implementation without result storage
 */
template <>
class CoroTask<void> {
   public:
    struct promise_type : PromiseBase {
        std::exception_ptr exception_;

        CoroTask<void> get_return_object() {
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

        void return_void() noexcept {}

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
    using value_type = void;
    using result_type = void;

    explicit CoroTask(std::coroutine_handle<promise_type> h)
        : coro_handle_(h) {}

    ~CoroTask() {
        if (coro_handle_) {
            coro_handle_.destroy();
        }
    }

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

    bool await_ready() const noexcept { return coro_handle_.done(); }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> awaiting_coro) noexcept {
        coro_handle_.promise().continuation_ = awaiting_coro;

        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            auto* awaiting_root = awaiting_coro.promise().get_root_promise();
            coro_handle_.promise().set_root_promise(awaiting_root);
        }

        if (coro_handle_.done()) {
            return awaiting_coro;
        }

        return coro_handle_;
    }

    void await_resume() {
        if (coro_handle_.promise().exception_) {
            std::rethrow_exception(coro_handle_.promise().exception_);
        }
    }

    bool done() const noexcept { return coro_handle_ && coro_handle_.done(); }

    void resume() {
        if (coro_handle_ && !coro_handle_.done()) {
            coro_handle_.resume();
        }
    }

    void get() {
        SyncScope sync;
        while (coro_handle_ && !coro_handle_.done()) {
            coro_handle_.resume();
        }
        await_resume();
    }

    bool has_exception() const noexcept {
        return coro_handle_ && coro_handle_.promise().exception_ != nullptr;
    }

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
    // Combinators and syntactic sugar (void specialization)
    // ========================================================================

    /**
     * Chain operation using then() - execute function after this task
     * @param func Function to execute (void -> U)
     * @return New CoroTask<U> with result
     *
     * Usage:
     * @code
     * auto result = co_await do_work_async()
     *     .then([]() { return 42; })
     *     .then([](int x) { return std::to_string(x); });
     * @endcode
     */
    template <typename Func>
    auto then(Func&& func) && -> CoroTask<std::invoke_result_t<Func>> {
        return [](CoroTask<void> self,
                  auto f) -> CoroTask<std::invoke_result_t<decltype(f)>> {
            using U = std::invoke_result_t<decltype(f)>;
            co_await std::move(self);
            if constexpr (std::is_void_v<U>) {
                f();
                co_return;
            } else {
                co_return f();
            }
        }(std::move(*this), std::forward<Func>(func));
    }

    /**
     * Tap operation - execute side effect after this task completes
     * @param func Inspection function (void -> void)
     * @return CoroTask<void>
     *
     * Usage:
     * @code
     * co_await do_work_async()
     *     .tap([]() { std::cout << "Work done\n"; })
     *     .then([]() { return 42; });
     * @endcode
     */
    template <typename Func>
    auto tap(Func&& func) && -> CoroTask<void> {
        return [](CoroTask<void> self, auto f) -> CoroTask<void> {
            co_await std::move(self);
            f();
            co_return;
        }(std::move(*this), std::forward<Func>(func));
    }

    /**
     * Operator> for chaining (same as then())
     * @param func Function to execute
     * @return Transformed CoroTask
     *
     * Usage:
     * @code
     * auto result = co_await do_work_async()
     *     > []() { return 42; }
     *     > [](int x) { return x * 2; };
     * @endcode
     */
    template <typename Func>
    auto operator>(Func&& func) && -> CoroTask<std::invoke_result_t<Func>> {
        return std::move(*this).then(std::forward<Func>(func));
    }

    /**
     * Operator< for reverse composition
     * @param func Function to execute after task
     * @return Transformed CoroTask
     */
    template <typename Func>
    friend auto operator<(Func&& func, CoroTask<void>&& task)
        -> CoroTask<std::invoke_result_t<Func>> {
        return [](CoroTask<void> self,
                  auto f) -> CoroTask<std::invoke_result_t<decltype(f)>> {
            using U = std::invoke_result_t<decltype(f)>;
            co_await std::move(self);
            if constexpr (std::is_void_v<U>) {
                f();
                co_return;
            } else {
                co_return f();
            }
        }(std::move(task), std::forward<Func>(func));
    }

    /**
     * Operator& for parallel composition (AND) - run both tasks sequentially
     * @param other Second task to run
     * @return CoroTask<U> with result from second task
     *
     * Note: Since both tasks are void, we return the result of the second task.
     * For true parallel execution, use CoroScope::spawn().
     *
     * Usage:
     * @code
     * auto result = co_await (void_task1() & value_task2());
     * @endcode
     */
    template <typename U>
    friend auto operator&(CoroTask<void>&& lhs, CoroTask<U>&& rhs)
        -> CoroTask<U> {
        return [](CoroTask<void> left, CoroTask<U> right) -> CoroTask<U> {
            co_await std::move(left);
            co_return co_await std::move(right);
        }(std::move(lhs), std::move(rhs));
    }

    /**
     * Operator| for OR/fallback composition - try first, fall back to second
     * @param fallback Fallback task to run if this task fails
     * @return CoroTask<void>
     *
     * Usage:
     * @code
     * co_await (primary_task() | fallback_task());
     * @endcode
     */
    friend auto operator|(CoroTask<void>&& primary, CoroTask<void>&& fallback)
        -> CoroTask<void> {
        return [](CoroTask<void> prim, CoroTask<void> fall) -> CoroTask<void> {
            std::exception_ptr primary_exception;
            try {
                co_await std::move(prim);
                co_return;
            } catch (...) {
                primary_exception = std::current_exception();
            }
            if (primary_exception) {
                co_await std::move(fall);
                co_return;
            }
            throw std::logic_error("Unreachable code in operator| reached");
        }(std::move(primary), std::move(fallback));
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_TASK_H
