#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/io_awaitable.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/task_future.h>

#include <any>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace dftracer::utils {

class Task;
class Scheduler;
class Executor;
class TaskScope;

/**
 * TaskContext - Context provided to tasks during execution
 *
 * Features:
 * - Spawn async tasks on worker threads (spawn)
 * - Offload I/O operations to I/O thread pool (spawn_io)
 * - Wait for all spawned tasks (join_all)
 *
 * Design Philosophy:
 * - All parallelism and concurrency accessed through TaskContext
 * - No thread-local globals or manual executor access
 * - Clean, consistent API
 *
 * Usage:
 * @code
 * auto task = make_task([](TaskContext& ctx) -> coro::CoroTask<void> {
 *     // Spawn parallel tasks
 *     ctx.spawn([](TaskContext& ctx) -> coro::CoroTask<int> {
 *         co_return compute();
 *     });
 *
 *     // Offload I/O
 *     auto data = co_await ctx.spawn_io([&]() {
 *         return read_file("data.bin");
 *     });
 *
 *     // Wait for all spawned tasks
 *     co_await ctx.join_all();
 *     co_return;
 * });
 * @endcode
 */
class TaskContext {
   private:
    Scheduler* scheduler_;
    TaskIndex current_task_id_;
    Executor* executor_;  // Direct reference for I/O access

    // Track spawned tasks for join_all()
    std::vector<TaskFuture<std::any>> spawned_tasks_;

    // Cancellation support
    std::shared_ptr<std::atomic<bool>> cancellation_requested_{
        std::make_shared<std::atomic<bool>>(false)};

    // Untracked spawn - used by TaskScope for structured concurrency
    TaskFuture<std::any> spawn_untracked(std::shared_ptr<Task> task,
                                         const std::any& input = {});

    template <typename Func>
    auto spawn_untracked(Func&& func) -> TaskFuture<
        typename std::invoke_result_t<Func, TaskContext&>::value_type>;

    friend class TaskScope;

   public:
    /**
     * Constructor
     * @param scheduler Pointer to scheduler
     * @param current_task_id ID of the currently executing task
     * @param executor Pointer to executor (for I/O operations)
     */
    TaskContext(Scheduler* scheduler, TaskIndex current_task_id,
                Executor* executor)
        : scheduler_(scheduler),
          current_task_id_(current_task_id),
          executor_(executor) {}

    // ========================================================================
    // Task Parallelism - spawn tasks on worker threads
    // ========================================================================

    /**
     * Spawn async task with automatic return type deduction
     * Task is tracked and can be awaited via join_all()
     *
     * @param func Lambda returning CoroTask<T>: [](TaskContext& ctx) ->
     * CoroTask<T> { ... }
     * @return TaskFuture<T> for type-safe awaiting
     *
     * Examples:
     * @code
     * // Void task
     * ctx.spawn([](TaskContext& ctx) -> coro::CoroTask<void> {
     *     expensive_work();
     *     co_return;
     * });
     *
     * // Typed task
     * auto future = ctx.spawn([](TaskContext& ctx) -> coro::CoroTask<int> {
     *     co_return 42;
     * });
     * int result = co_await future;  // Type-safe
     * @endcode
     */
    template <typename Func>
    auto spawn(Func&& func) -> TaskFuture<
        typename std::invoke_result_t<Func, TaskContext&>::value_type> {
        auto future = spawn_untracked(std::forward<Func>(func));
        spawned_tasks_.push_back(
            future);  // Implicit conversion to TaskFuture<std::any>
        return future;
    }

    /**
     * Spawn task from explicit Task object
     * Task is tracked and can be awaited via join_all()
     */
    TaskFuture<std::any> spawn(std::shared_ptr<Task> task,
                               const std::any& input = {}) {
        auto future = spawn_untracked(task, input);
        spawned_tasks_.push_back(future);
        return future;
    }

    /**
     * Spawn task from explicit Task object with typed input and result
     * Provides cleaner API without manual std::any wrapping
     *
     * @tparam OutputType Expected output type
     * @tparam InputType Input type (automatically deduced)
     * @param task The task to spawn
     * @param input Input data for the task
     * @return TaskFuture<OutputType> that can be co_await-ed for typed result
     *
     * Examples:
     * @code
     * auto task = make_task([](TaskContext& ctx, int x) -> CoroTask<int> {
     *     co_return x * 2;
     * });
     * auto future = ctx.spawn<int>(task, 21);  // No std::any needed!
     * int result = co_await future;  // result = 42
     * @endcode
     */
    template <typename OutputType, typename InputType>
    TaskFuture<OutputType> spawn(std::shared_ptr<Task> task,
                                 InputType&& input) {
        TaskFuture<std::any> any_future =
            spawn(task, std::any{std::forward<InputType>(input)});
        return TaskFuture<OutputType>(any_future.get_task(),
                                      any_future.get_task_id(), scheduler_,
                                      cancellation_requested_);
    }

    /**
     * Wait for all spawned tasks to complete
     * Returns awaitable that suspends until all pending tasks done
     * Clears spawned task list after joining
     *
     * @return CoroTask<void> that can be co_await-ed
     */
    coro::CoroTask<void> join_all();

    // ========================================================================
    // I/O Operations - offload to I/O thread pool
    // ========================================================================

    /**
     * Spawn I/O operation on dedicated I/O thread pool
     *
     * Offloads I/O operations (reads, writes, syscalls) to I/O thread pool.
     * Suspends coroutine, doesn't block worker threads.
     * Automatically falls back to inline execution if async I/O not enabled.
     *
     * Use for: File I/O, network I/O, blocking syscalls
     * Avoid for: CPU-heavy computations (use spawn() instead)
     *
     * @param func Lambda performing I/O operation: [&]() { return
     * io_operation(); }
     * @return IOAwaitable<T> that can be co_await-ed
     *
     * Examples:
     * @code
     * // File I/O
     * auto data = co_await ctx.spawn_io([&]() {
     *     return read_file("data.bin");
     * });
     *
     * // Network I/O
     * auto response = co_await ctx.spawn_io([&]() {
     *     return fetch_url("https://api.example.com");
     * });
     * @endcode
     */
    template <typename Func>
    auto spawn_io(Func&& func)
        -> coro::IOAwaitable<std::invoke_result_t<Func>> {
        using ReturnType = std::invoke_result_t<Func>;

        if (!has_async_io()) {
            // No async I/O available
            // execute inline and return ready awaitable
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    func();
                    return coro::IOAwaitable<void>::make_ready();
                } else {
                    return coro::IOAwaitable<ReturnType>::make_ready(func());
                }
            } catch (...) {
                return coro::IOAwaitable<ReturnType>::make_exceptional(
                    std::current_exception());
            }
        }

        // Async I/O available
        // create awaitable that will be executed on I/O thread
        return coro::IOAwaitable<ReturnType>(std::forward<Func>(func));
    }

    /**
     * Check if async I/O is available
     *
     * Returns true if pipeline was configured with .with_io_threads()
     * If false, spawn_io() executes inline and returns immediately
     */
    bool has_async_io() const;

    /**
     * Async receive from channel (offloads blocking receive to I/O thread)
     *
     * This method wraps channel.receive() and offloads it to the I/O thread
     * pool if available, preventing worker threads from blocking.
     *
     * Use for: Receiving from channels in coroutine-based tasks
     *
     * @param channel Channel to receive from
     * @return IOAwaitable<std::optional<T>> - Some(value) if received, None if
     * channel closed
     *
     * Examples:
     * @code
     * // Async receive in a task
     * auto consumer = make_task([&](TaskContext& ctx) -> coro::CoroTask<void> {
     *     while (auto item = co_await ctx.receive_async(channel)) {
     *         process(*item);
     *     }
     *     co_return;
     * });
     * @endcode
     */
    template <typename T>
    auto receive_async(coro::Channel<T>& channel)
        -> coro::IOAwaitable<std::optional<T>> {
        return spawn_io([&channel]() -> std::optional<T> {
            T item;
            if (channel.receive(item)) {
                return std::optional<T>(std::move(item));
            }
            return std::nullopt;
        });
    }

    /**
     * Async receive from channel (shared_ptr version)
     *
     * Accepts a shared_ptr to the channel, ensuring the channel
     * outlives the I/O operation.
     *
     * @param channel shared_ptr to the channel
     * @return IOAwaitable that resolves to std::optional<T>
     */
    template <typename T>
    auto receive_async(std::shared_ptr<coro::Channel<T>> channel)
        -> coro::IOAwaitable<std::optional<T>> {
        return spawn_io([channel]() -> std::optional<T> {
            T item;
            if (channel->receive(item)) {
                return std::optional<T>(std::move(item));
            }
            return std::nullopt;
        });
    }

    // ========================================================================
    // Existing methods
    // ========================================================================

    /**
     * Get current task ID
     */
    TaskIndex current() const { return current_task_id_; }

    /**
     * Get scheduler reference (for advanced use cases)
     */
    Scheduler* get_scheduler() const { return scheduler_; }

    /**
     * Get executor reference (for advanced use cases)
     */
    Executor* get_executor() const { return executor_; }

    // ========================================================================
    // Cancellation Support
    // ========================================================================

    /**
     * Check if cancellation has been requested
     * Tasks should periodically check this and exit gracefully if true
     *
     * @return true if cancellation requested, false otherwise
     */
    bool is_cancellation_requested() const {
        return cancellation_requested_->load(std::memory_order_acquire);
    }

    /**
     * Request cancellation of this task
     * This is cooperative cancellation - tasks must check
     * is_cancellation_requested()
     */
    void request_cancellation() {
        cancellation_requested_->store(true, std::memory_order_release);
    }

    /**
     * Get the cancellation token (for sharing with child contexts)
     */
    std::shared_ptr<std::atomic<bool>> get_cancellation_token() const {
        return cancellation_requested_;
    }

    // ========================================================================
    // Structured Concurrency - TaskScope
    // ========================================================================

    /**
     * Create structured scope that auto-joins all spawned tasks
     *
     * Provides structured concurrency guarantees, ensuring all spawned tasks
     * complete before the scope exits. Prevents lifetime bugs when using
     * channels or other shared resources.
     *
     * @param scope_func Lambda to execute with TaskScope
     *                   Signature: [](TaskScope& scope) { ... }
     * @return CoroTask<void> that can be co_await-ed
     *
     * Usage:
     * @code
     * // Pattern 1: Simple parallel tasks
     * co_await ctx.scope([&](TaskScope& scope) {
     *     scope.spawn(task1);
     *     scope.spawn(task2);
     *     // Auto-waits for task1 and task2 before returning
     * });
     *
     * // Pattern 2: Producer-consumer pattern
     * co_await ctx.scope([&](TaskScope& scope) {
     *     auto channel = coro::Channel<Data>(100);
     *
     *     scope.spawn_producer(channel, [](TaskContext& ctx) ->
     * coro::Generator<Data> {
     *         for (int i = 0; i < 100; i++) {
     *             auto data = co_await ctx.spawn_io([i]() { return
     * read_file(i);
     * });
     *             co_yield data;
     *         }
     *     });
     *
     *     scope.spawn_consumers(channel, 8, [](TaskContext& ctx, Data d) ->
     * coro::CoroTask<void> {
     *         process(d);
     *         co_return;
     *     });
     *     // Channel is safe: all tasks finish before scope exits
     * });
     * @endcode
     */
    template <typename Func>
        requires std::is_invocable_r_v<coro::CoroTask<void>, Func, TaskScope&>
    coro::CoroTask<void> scope(Func&& scope_func);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H
