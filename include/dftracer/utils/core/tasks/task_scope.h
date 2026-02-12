#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_SCOPE_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_SCOPE_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/task_future.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace dftracer::utils {

// Forward declarations
class TaskContext;

/**
 * TaskScope - Structured concurrency for managing groups of spawned tasks
 *
 * Provides structured concurrency guarantees,
 * ensuring all spawned tasks complete before the scope exits.
 *
 * Particularly useful for producer-consumer patterns with channels.
 *
 *
 * Problem (without TaskScope):
 * @code
 * // UNSAFE: Channel destroyed while tasks still running
 * auto task = make_task([](TaskContext& ctx) -> coro::CoroTask<void> {
 *     auto channel = coro::Channel<int>(100);
 *
 *     ctx.spawn([&channel](TaskContext& ctx) -> coro::CoroTask<void> {
 *         while (auto val = co_await channel.receive()) {
 *             process(*val);
 *         }
 *     });
 *
 *     co_return;  // --> Task finishes, channel destroyed, spawned task
 * crashes!
 * });
 * @endcode
 *
 * Solution (with TaskScope):
 * @code
 * // Scope waits for all tasks
 * auto task = make_task([](TaskContext& ctx) -> coro::CoroTask<void> {
 *     auto channel = coro::Channel<int>(100);
 *
 *     co_await ctx.scope([&](TaskScope& scope) {
 *         scope.spawn([&channel](TaskContext& ctx) -> coro::CoroTask<void> {
 *             while (auto val = co_await channel.receive()) {
 *                 process(*val);
 *             }
 *         });
 *         // Scope auto-waits before returning
 *     });
 *
 *     // Channel only destroyed here, after all tasks finish
 *     co_return;
 * });
 * @endcode
 *
 * Usage Patterns:
 * @code
 * // Pattern 1: Auto-join with ctx.scope()
 * co_await ctx.scope([&](TaskScope& scope) {
 *     scope.spawn(task1);
 *     scope.spawn(task2);
 *     // Auto-waits for task1 and task2 before returning
 * });
 *
 * // Pattern 2: Manual control with explicit join()
 * TaskScope scope(&ctx);
 * scope.spawn(task1);
 * scope.spawn(task2);
 * co_await scope.join();
 *
 * // Pattern 3: Producer-consumer
 * co_await ctx.scope([&](TaskScope& scope) {
 *     auto channel = coro::Channel<Data>(100);
 *
 *     scope.spawn_producer(channel, [](TaskContext& ctx) ->
 * coro::Generator<Data> {
 *         for (int i = 0; i < 100; i++) {
 *             auto data = co_await ctx.spawn_io([i]() { return read_file(i);
 * }); co_yield data;
 *         }
 *     });
 *
 *     scope.spawn_consumers(channel, 8, [](TaskContext& ctx, Data d) ->
 * coro::CoroTask<void> { process(d); co_return;
 *     });
 * });
 * @endcode
 */
class TaskScope {
   private:
    TaskContext* ctx_;
    std::vector<TaskFuture<std::any>> futures_;
    bool joined_ = false;

   public:
    /**
     * Constructor
     * @param ctx TaskContext for spawning tasks
     */
    explicit TaskScope(TaskContext* ctx) : ctx_(ctx) {}

    /**
     * Destructor
     *
     * Ensures join() was called
     */
    ~TaskScope() {
        // With shared_ptr channels, tasks can outlive the scope
        // So we just log a warning if join wasn't called
        if (!joined_ && !futures_.empty()) {
            DFTRACER_UTILS_LOG_WARN(
                "TaskScope destroyed without join()! %zu tasks still pending.",
                futures_.size());
        }
    }

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    // ========================================================================
    // Basic Spawning
    // ========================================================================

    /**
     * Spawn task on compute thread pool
     * Task is tracked by scope and will be waited for in join()
     *
     * @param func Lambda returning CoroTask<T>
     * @return TaskFuture<T> for type-safe awaiting
     *
     * Example:
     * @code
     * auto f = scope.spawn([](TaskContext& ctx) -> coro::CoroTask<int> {
     *     co_return 42;
     * });
     * int result = co_await f;  // Type-safe
     * @endcode
     */
    template <typename Func>
    auto spawn(Func&& func) -> TaskFuture<
        typename std::invoke_result_t<Func, TaskContext&>::value_type> {
        // Use untracked spawn - not added to TaskContext::spawned_tasks_
        auto typed_future = ctx_->spawn_untracked(std::forward<Func>(func));

        // Explicitly convert to TaskFuture<std::any> using conversion
        // constructor
        TaskFuture<std::any> any_future(typed_future);
        futures_.push_back(std::move(any_future));

        // Return typed future to caller
        return typed_future;
    }

    // ========================================================================
    // Producer-Consumer Convenience Wrappers
    // ========================================================================

    /**
     * Spawn producer that feeds Generator<T> into Channel<T>
     *
     * Uses synchronous Generator (C++23 std::generator-like).
     * Good for: Pure computation, or when you manually control async via
     * ctx.spawn_io()
     *
     * Wrapper around spawn() - runs on COMPUTE thread pool
     * User can delegate I/O via ctx.spawn_io() inside the generator
     * Automatically manages producer_guard for proper channel closure
     *
     * @param channel The channel to send items to (by reference)
     * @param generator_func Function returning Generator<T>
     *                       Signature: (TaskContext&) -> Generator<T>
     *
     * Example with mixed I/O + CPU (manual async control):
     * @code
     * scope.spawn_producer(chunks, [](TaskContext& ctx) ->
     * coro::Generator<Chunk> {
     *     for (int i = 0; i < 100; i++) {
     *         // I/O: Manually delegate to I/O pool via spawn_io
     *         auto raw = co_await ctx.spawn_io([i]() {
     *             return read_file(i);  // Runs on I/O thread
     *         });
     *
     *         // CPU: Process on this thread
     *         auto chunk = decompress_and_parse(raw);
     *
     *         co_yield chunk;
     *     }
     * });
     * @endcode
     *
     * Note: If you need native async iteration (co_await per value),
     *       use spawn_async_producer() with AsyncGenerator instead.
     */
    template <typename T, typename Func>
    void spawn_producer(coro::Channel<T>& channel, Func&& generator_func) {
        channel.register_producer();
        spawn([&channel, func = std::forward<Func>(generator_func)](
                  TaskContext& ctx) -> coro::CoroTask<void> {
            auto guard = channel.adopt_producer();
            coro::Generator<T> gen = func(ctx);

            for (auto item : gen) {
                channel.send_blocking(std::move(item));
            }

            co_return;
        });
    }

    /**
     * Spawn producer that feeds Generator<T> into Channel<T> (shared_ptr
     * version)
     *
     * This version accepts a shared_ptr to the channel, ensuring the channel
     * outlives the spawned tasks. Useful when the channel is created inside
     * the scope lambda and needs to survive until all tasks complete.
     *
     * @param channel The channel to send items to (shared_ptr)
     * @param generator_func Function returning Generator<T>
     *
     * Example:
     * @code
     * co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
     *     auto channel = coro::make_channel<Chunk>(100);
     *     scope.spawn_producer(channel, [](TaskContext& ctx) ->
     * coro::Generator<Chunk> {
     *         // ... produce items
     *     });
     *     // Channel stays alive via shared_ptr even after lambda returns
     *     co_return;
     * });
     * @endcode
     */
    template <typename T, typename Func>
    void spawn_producer(std::shared_ptr<coro::Channel<T>> channel,
                        Func&& generator_func) {
        channel->register_producer();
        spawn([channel, func = std::forward<Func>(generator_func)](
                  TaskContext& ctx) -> coro::CoroTask<void> {
            auto guard = channel->adopt_producer();
            coro::Generator<T> gen = func(ctx);

            for (auto item : gen) {
                channel->send_blocking(std::move(item));
            }

            co_return;
        });
    }

    /**
     * Spawn async producer that feeds AsyncGenerator<T> into Channel<T>
     *
     * Wrapper around spawn() - runs on COMPUTE thread pool
     * AsyncGenerator supports async value production (co_await per iteration)
     * Automatically manages producer lifecycle via adopt_producer()
     *
     * @param channel The channel to send items to (by reference)
     * @param async_generator_func Function returning AsyncGenerator<T>
     *                             Signature: (TaskContext&) ->
     * AsyncGenerator<T>
     *
     * Example with async I/O:
     * @code
     * scope.spawn_async_producer(chunks, [](TaskContext& ctx) ->
     * coro::AsyncGenerator<Chunk> {
     *     for (int i = 0; i < 100; i++) {
     *         // Async I/O operation directly in generator
     *         auto raw = co_await ctx.spawn_io([i]() {
     *             return read_file(i);
     *         });
     *         auto chunk = decompress_and_parse(raw);
     *         co_yield chunk;
     *     }
     * });
     * @endcode
     */
    template <typename T, typename Func>
    void spawn_async_producer(coro::Channel<T>& channel,
                              Func&& async_generator_func) {
        channel.register_producer();
        spawn([&channel, func = std::forward<Func>(async_generator_func)](
                  TaskContext& ctx) -> coro::CoroTask<void> {
            auto guard = channel.adopt_producer();
            coro::AsyncGenerator<T> gen = func(ctx);

            while (auto item = co_await gen.next()) {
                channel.send_blocking(std::move(*item));
            }

            co_return;
        });
    }

    /**
     * Spawn async producer that feeds AsyncGenerator<T> into Channel<T>
     * (shared_ptr version)
     *
     * This version accepts a shared_ptr to the channel, ensuring the channel
     * outlives the spawned tasks.
     *
     * @param channel The channel to send items to (shared_ptr)
     * @param async_generator_func Function returning AsyncGenerator<T>
     *
     * Example:
     * @code
     * co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
     *     auto channel = coro::make_channel<Chunk>(100);
     *     scope.spawn_async_producer(channel, [](TaskContext& ctx) ->
     * coro::AsyncGenerator<Chunk> {
     *         // ... async produce items
     *     });
     *     co_return;
     * });
     * @endcode
     */
    template <typename T, typename Func>
    void spawn_async_producer(std::shared_ptr<coro::Channel<T>> channel,
                              Func&& async_generator_func) {
        channel->register_producer();
        spawn([channel, func = std::forward<Func>(async_generator_func)](
                  TaskContext& ctx) -> coro::CoroTask<void> {
            auto guard = channel->adopt_producer();
            coro::AsyncGenerator<T> gen = func(ctx);

            while (auto item = co_await gen.next()) {
                channel->send_blocking(std::move(*item));
            }

            co_return;
        });
    }

    /**
     * Spawn N producers that each run a coroutine producing into Channel<T>
     *
     * Pre-registers all producers, then spawns N coroutines each with an
     * adopt_producer() guard for automatic cleanup.
     *
     * @param channel The channel to send items to (shared_ptr)
     * @param count Number of parallel producer instances
     * @param producer_func Function to run in each producer
     *                      Signature: (TaskContext&, std::size_t index) ->
     *                      CoroTask<void>
     *
     * Example:
     * @code
     * scope.spawn_producers(channel, 4,
     *     [&](TaskContext& ctx, std::size_t idx) -> coro::CoroTask<void> {
     *         for (auto& item : get_items_for(idx))
     *             channel->send_blocking(std::move(item));
     *         co_return;
     *     });
     * @endcode
     */
    template <typename T, typename Func>
    void spawn_producers(std::shared_ptr<coro::Channel<T>> channel,
                         std::size_t count, Func&& producer_func) {
        channel->register_producers(count);
        for (std::size_t i = 0; i < count; ++i) {
            spawn([channel, func = producer_func,
                   i](TaskContext& ctx) -> coro::CoroTask<void> {
                auto guard = channel->adopt_producer();
                co_await func(ctx, i);
                co_return;
            });
        }
    }

    /**
     * Spawn N producers (reference version)
     */
    template <typename T, typename Func>
    void spawn_producers(coro::Channel<T>& channel, std::size_t count,
                         Func&& producer_func) {
        channel.register_producers(count);
        for (std::size_t i = 0; i < count; ++i) {
            spawn([&channel, func = producer_func,
                   i](TaskContext& ctx) -> coro::CoroTask<void> {
                auto guard = channel.adopt_producer();
                co_await func(ctx, i);
                co_return;
            });
        }
    }

    /**
     * Spawn N consumers that drain Channel<T>
     *
     * Wrapper around spawn() - each consumer runs on COMPUTE thread pool
     * User can delegate I/O via ctx.spawn_io() inside the consumer
     *
     * @param channel The channel to receive items from (by reference)
     * @param count Number of parallel consumer instances
     * @param consumer_func Function to process each item
     *                      Signature: (TaskContext&, T) -> CoroTask<void>
     *
     * Example with mixed CPU + I/O:
     * @code
     * scope.spawn_consumers(results, 32, [](TaskContext& ctx, Result r) ->
     * coro::CoroTask<void> {
     *     // CPU: Process on this thread
     *     auto output = analyze(r);
     *
     *     // I/O: Delegate write to I/O pool (ctx available!)
     *     co_await ctx.spawn_io([output]() {
     *         write_to_disk(output);  // Runs on I/O thread
     *     });
     *
     *     co_return;
     * });
     * @endcode
     */
    template <typename T, typename Func>
    void spawn_consumers(coro::Channel<T>& channel, std::size_t count,
                         Func&& consumer_func) {
        for (std::size_t i = 0; i < count; i++) {
            spawn([&channel, func = consumer_func](
                      TaskContext& ctx) -> coro::CoroTask<void> {
                while (auto item = co_await ctx.receive_async(channel)) {
                    co_await func(ctx, std::move(*item));
                }
                co_return;
            });
        }
    }

    /**
     * Spawn N consumers that drain Channel<T> (shared_ptr version)
     *
     * This version accepts a shared_ptr to the channel, ensuring the channel
     * outlives the spawned tasks.
     *
     * @param channel The channel to receive items from (shared_ptr)
     * @param count Number of parallel consumer instances
     * @param consumer_func Function to process each item
     *
     * Example:
     * @code
     * co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
     *     auto channel = coro::make_channel<Result>(100);
     *     scope.spawn_consumers(channel, 32, [](TaskContext& ctx, Result r) ->
     * coro::CoroTask<void> {
     *         // ... process result
     *         co_return;
     *     });
     *     co_return;
     * });
     * @endcode
     */
    template <typename T, typename Func>
    void spawn_consumers(std::shared_ptr<coro::Channel<T>> channel,
                         std::size_t count, Func&& consumer_func) {
        for (std::size_t i = 0; i < count; i++) {
            spawn([channel, func = consumer_func](
                      TaskContext& ctx) -> coro::CoroTask<void> {
                while (auto item = co_await ctx.receive_async(channel)) {
                    co_await func(ctx, std::move(*item));
                }
                co_return;
            });
        }
    }

    // ========================================================================
    // Transform (Consumer-Producer Bridge)
    // ========================================================================

    /**
     * Spawn N workers that consume from one channel, transform, and produce
     * into another channel (1:1 mapping).
     *
     * Handles producer registration and RAII cleanup on the output channel
     * automatically. Each worker loops over the input channel and sends
     * the transformed result to the output channel.
     *
     * @param input Input channel to consume from (shared_ptr)
     * @param output Output channel to produce into (shared_ptr)
     * @param count Number of parallel transform workers
     * @param transform_func Function to transform each item
     *                       Signature: (TaskContext&, TIn) -> CoroTask<TOut>
     *
     * Example:
     * @code
     * scope.spawn_transforms(chunk_chan, result_chan, 8,
     *     [](TaskContext& ctx, Chunk chunk) -> coro::CoroTask<Result> {
     *         auto result = process(chunk);
     *         co_return result;
     *     });
     * @endcode
     */
    template <typename TIn, typename TOut, typename Func>
    void spawn_transforms(std::shared_ptr<coro::Channel<TIn>> input,
                          std::shared_ptr<coro::Channel<TOut>> output,
                          std::size_t count, Func&& transform_func) {
        output->register_producers(count);
        for (std::size_t i = 0; i < count; ++i) {
            spawn([input, output, func = transform_func](
                      TaskContext& ctx) -> coro::CoroTask<void> {
                auto guard = output->adopt_producer();
                while (auto item = co_await ctx.receive_async(input)) {
                    auto result = co_await func(ctx, std::move(*item));
                    output->send_blocking(std::move(result));
                }
                co_return;
            });
        }
    }

    /**
     * Spawn N transform workers (reference version)
     */
    template <typename TIn, typename TOut, typename Func>
    void spawn_transforms(coro::Channel<TIn>& input,
                          coro::Channel<TOut>& output, std::size_t count,
                          Func&& transform_func) {
        output.register_producers(count);
        for (std::size_t i = 0; i < count; ++i) {
            spawn([&input, &output, func = transform_func](
                      TaskContext& ctx) -> coro::CoroTask<void> {
                auto guard = output.adopt_producer();
                while (auto item = co_await ctx.receive_async(input)) {
                    auto result = co_await func(ctx, std::move(*item));
                    output.send_blocking(std::move(result));
                }
                co_return;
            });
        }
    }

    // ========================================================================
    // Waiting
    // ========================================================================

    /**
     * Wait for all spawned tasks to complete
     * Must be called before TaskScope is destroyed
     *
     * @return CoroTask<void> that can be co_await-ed
     *
     * Example:
     * @code
     * TaskScope scope(&ctx);
     * scope.spawn(task1);
     * scope.spawn(task2);
     * co_await scope.join();  // Wait for all tasks
     * @endcode
     */
    coro::CoroTask<void> join() {
        joined_ = true;
        auto awaitable = coro::when_all(futures_);
        co_await awaitable;
        futures_.clear();
        co_return;
    }

    /**
     * Get number of spawned tasks tracked by this scope
     */
    std::size_t size() const { return futures_.size(); }

    /**
     * Check if join() has been called
     */
    bool is_joined() const { return joined_; }
};

// ============================================================================
// TaskContext::scope() implementation
// Must be here after TaskScope is fully defined
// ============================================================================

template <typename Func>
    requires std::is_invocable_r_v<coro::CoroTask<void>, Func, TaskScope&>
inline coro::CoroTask<void> TaskContext::scope(Func&& scope_func) {
    TaskScope scope(this);
    co_await scope_func(scope);
    co_await scope.join();
    co_return;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_SCOPE_H
