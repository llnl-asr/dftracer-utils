#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/generator.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/tasks/task_scope.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace dftracer::utils;

// ============================================================================
// gcc11_bandaid: Helper coroutine functions to avoid GCC 11 ICE/SIGSEGV
// with nested coroutine lambdas in scope.spawn()
// ============================================================================

static coro::CoroTask<int> scope_spawn_increment_helper(TaskContext&,
                                                        std::atomic<int>& count,
                                                        int /*i*/) {
    ++count;
    co_return 0;
}

static coro::CoroTask<int> scope_spawn_add_helper(TaskContext&,
                                                  std::atomic<int>& sum,
                                                  int value) {
    sum.fetch_add(value);
    co_return value;
}

// gcc11_bandaid: Scope callback helpers to avoid nested coroutine lambdas
// These are named coroutine functions that can be called from regular lambdas
// passed to ctx.scope(), avoiding the nested coroutine lambda pattern that
// causes GCC 11 SIGSEGV/ICE.

static coro::CoroTask<void> scope_increment_n_helper(TaskScope& scope,
                                                     std::atomic<int>& count,
                                                     int n) {
    // IMPORTANT: Capture pointer by value, not reference by reference
    // Coroutine frame may be destroyed before spawned tasks complete
    auto* count_ptr = &count;
    for (int i = 0; i < n; ++i) {
        scope.spawn([count_ptr, i](TaskContext& inner_ctx) {
            return scope_spawn_increment_helper(inner_ctx, *count_ptr, i);
        });
    }
    co_return;
}

static coro::CoroTask<void> scope_add_range_helper(TaskScope& scope,
                                                   std::atomic<int>& sum,
                                                   int start, int end) {
    // IMPORTANT: Capture pointer by value, not reference by reference
    // Coroutine frame may be destroyed before spawned tasks complete
    auto* sum_ptr = &sum;
    for (int i = start; i <= end; ++i) {
        scope.spawn([sum_ptr, i](TaskContext& inner_ctx) {
            return scope_spawn_add_helper(inner_ctx, *sum_ptr, i);
        });
    }
    co_return;
}

// gcc11_bandaid: Named coroutine helpers for producer/consumer patterns

// Generator for sync producer (yields i * 10 for i in [0, count))
static coro::Generator<int> producer_gen_multiply_10(TaskContext&, int count) {
    for (int i = 0; i < count; ++i) {
        co_yield i * 10;
    }
}

// Generator for simple range producer
static coro::Generator<int> producer_gen_range(TaskContext&, int count) {
    for (int i = 0; i < count; ++i) {
        co_yield i;
    }
}

// Consumer that adds items to atomic sum
static coro::CoroTask<void> consumer_add_to_sum(TaskContext&,
                                                std::atomic<int>& sum,
                                                int item) {
    sum.fetch_add(item);
    co_return;
}

// Consumer that increments a counter
static coro::CoroTask<void> consumer_increment(TaskContext&,
                                               std::atomic<int>& count, int) {
    count++;
    co_return;
}

// Scope callback: sync producer with consumer that sums
static coro::CoroTask<void> scope_producer_consumer_sum_helper(
    TaskScope& scope, std::atomic<int>& sum, int count) {
    auto channel = coro::make_channel<int>(10);
    // IMPORTANT: Capture pointer by value for coroutine safety
    auto* sum_ptr = &sum;

    scope.spawn_producer(channel, [count](TaskContext& ctx) {
        return producer_gen_multiply_10(ctx, count);
    });

    scope.spawn_consumers(channel, 1, [sum_ptr](TaskContext& ctx, int item) {
        return consumer_add_to_sum(ctx, *sum_ptr, item);
    });

    co_return;
}

// Scope callback: producer with multiple consumers that count
static coro::CoroTask<void> scope_producer_multi_consumer_count_helper(
    TaskScope& scope, std::atomic<int>& count, int item_count,
    int consumer_count) {
    auto channel = coro::make_channel<int>(item_count);
    // IMPORTANT: Capture pointer by value for coroutine safety
    auto* count_ptr = &count;

    scope.spawn_producer(channel, [item_count](TaskContext& ctx) {
        return producer_gen_range(ctx, item_count);
    });

    scope.spawn_consumers(channel, consumer_count,
                          [count_ptr](TaskContext& ctx, int item) {
                              return consumer_increment(ctx, *count_ptr, item);
                          });

    co_return;
}

// ============================================================================
// Basic TaskScope Tests
// ============================================================================

TEST_CASE("TaskScope - Basic construction and spawning") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> task_count{0};
    auto* count_ptr = &task_count;

    auto parent_task = make_task(
        [count_ptr](TaskContext& ctx) -> coro::CoroTask<void> {
            // gcc11_bandaid: Use regular lambda that returns CoroTask
            // instead of nested coroutine lambda
            co_await ctx.scope([count_ptr](TaskScope& scope) {
                return scope_increment_n_helper(scope, *count_ptr, 3);
            });
            co_return;
        },
        "ParentWithScope");

    scheduler.schedule(parent_task);
    parent_task->wait();

    CHECK(task_count.load() == 3);

    executor.shutdown();
}

TEST_CASE("TaskScope - Automatic join via ctx.scope()") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};
    auto* sum_ptr = &sum;  // use pointer to avoid reference issues

    auto parent_task = make_task(
        [sum_ptr](TaskContext& ctx) -> coro::CoroTask<void> {
            // gcc11_bandaid: Use regular lambda that returns CoroTask
            co_await ctx.scope([sum_ptr](TaskScope& scope) {
                return scope_add_range_helper(scope, *sum_ptr, 1, 10);
            });

            co_return;
        },
        "AutoJoinScope");

    scheduler.schedule(parent_task);
    parent_task->wait();

    CHECK(sum.load() == 55);

    executor.shutdown();
}

TEST_CASE("TaskScope - More threads than tasks") {
    Executor executor(12);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};
    auto* sum_ptr = &sum;  // use pointer to avoid reference issues

    auto parent_task = make_task(
        [sum_ptr](TaskContext& ctx) -> coro::CoroTask<void> {
            // gcc11_bandaid: Use regular lambda that returns CoroTask
            co_await ctx.scope([sum_ptr](TaskScope& scope) {
                return scope_add_range_helper(scope, *sum_ptr, 1, 10);
            });
            co_return;
        },
        "MoreThreadsThanTasks");

    scheduler.schedule(parent_task);
    parent_task->wait();

    CHECK(sum.load() == 55);

    executor.shutdown();
}

TEST_CASE("TaskScope - Regression test for hang fix") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};
    auto* sum_ptr = &sum;  // use pointer to avoid reference issues

    auto parent_task = make_task(
        [sum_ptr](TaskContext& ctx) -> coro::CoroTask<void> {
            // gcc11_bandaid: Use regular lambda that returns CoroTask
            co_await ctx.scope([sum_ptr](TaskScope& scope) {
                return scope_add_range_helper(scope, *sum_ptr, 1, 5);
            });
            co_return;
        },
        "HangRegressionTest");

    scheduler.schedule(parent_task);
    parent_task->wait();

    CHECK(sum.load() == 15);

    executor.shutdown();
}

//============================================================================
// Producer-Consumer Pattern Tests
// ============================================================================

TEST_CASE("TaskScope - spawn_producer with synchronous Generator") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> items_received{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            // gcc11_bandaid: Use regular lambda that returns CoroTask
            co_await ctx.scope([&](TaskScope& scope) {
                return scope_producer_consumer_sum_helper(scope, items_received,
                                                          5);
            });

            co_return;
        },
        "ProducerConsumerSync");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Sum: 0 + 10 + 20 + 30 + 40 = 100
    CHECK(items_received.load() == 100);

    executor.shutdown();
}

TEST_CASE("TaskScope - spawn_consumers with multiple consumers") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> items_processed{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            // gcc11_bandaid: Use regular lambda that returns CoroTask
            co_await ctx.scope([&](TaskScope& scope) {
                return scope_producer_multi_consumer_count_helper(
                    scope, items_processed, 20, 4);
            });

            co_return;
        },
        "MultipleConsumers");

    scheduler.schedule(parent_task);
    parent_task->wait();

    CHECK(items_processed.load() == 20);

    executor.shutdown();
}

TEST_CASE("TaskScope - spawn_async_producer with AsyncGenerator and I/O") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                // Async producer with I/O operations
                scope.spawn_async_producer(
                    channel,
                    [](TaskContext& inner_ctx) -> coro::AsyncGenerator<int> {
                        for (int i = 1; i <= 10; ++i) {
                            // Async I/O operation
                            int value = co_await inner_ctx.spawn_io([i]() {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                                return i;
                            });
                            co_yield value;
                        }
                    });

                // Consumer
                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        sum.fetch_add(item);
                        co_return;
                    });

                co_return;
            });

            co_return;
        },
        "AsyncGeneratorWithIO");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Sum: 1+2+3+...+10 = 55
    CHECK(sum.load() == 55);

    executor.shutdown();
}

// ============================================================================
// Async Producer-Consumer Pattern Tests (with AsyncGenerator)
// ============================================================================

TEST_CASE("TaskScope - spawn_async_producer with AsyncGenerator") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> items_received{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                // Spawn async producer with AsyncGenerator
                scope.spawn_async_producer(
                    channel,
                    [](TaskContext& inner_ctx) -> coro::AsyncGenerator<int> {
                        for (int i = 0; i < 5; ++i) {
                            // Simulate async I/O using spawn_io
                            auto value = co_await inner_ctx.spawn_io([i]() {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(5));
                                return i * 10;  // 0, 10, 20, 30, 40
                            });
                            co_yield value;
                        }
                    });

                // Spawn consumer
                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        items_received.fetch_add(item);
                        co_return;
                    });

                co_return;
            });

            co_return;
        },
        "AsyncProducerConsumer");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Sum: 0 + 10 + 20 + 30 + 40 = 100
    CHECK(items_received.load() == 100);

    executor.shutdown();
}

TEST_CASE("TaskScope - AsyncGenerator with multiple async producers") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> total_items{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(20);

                // Spawn 2 async producers
                for (int p = 0; p < 2; ++p) {
                    scope.spawn_async_producer(
                        channel,
                        [p](TaskContext& inner_ctx)
                            -> coro::AsyncGenerator<int> {
                            for (int i = 0; i < 5; ++i) {
                                // Async delay
                                co_await inner_ctx.spawn_io([p, i]() {
                                    std::this_thread::sleep_for(
                                        std::chrono::milliseconds(2));
                                    return p * 100 + i;
                                });
                                co_yield p * 100 + i;
                            }
                        });
                }

                // Spawn consumer
                scope.spawn_consumers(
                    channel, 1, [&](TaskContext&, int) -> coro::CoroTask<void> {
                        total_items++;
                        co_return;
                    });
                co_return;
            });

            co_return;
        },
        "MultipleAsyncProducers");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // 2 producers * 5 items each = 10 items
    CHECK(total_items.load() == 10);

    executor.shutdown();
}

TEST_CASE("TaskScope - Generator produces correct sum") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sync_result{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                scope.spawn_producer(channel,
                                     [](TaskContext&) -> coro::Generator<int> {
                                         for (int i = 1; i <= 5; ++i) {
                                             co_yield i;
                                         }
                                     });

                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        sync_result.fetch_add(item);
                        co_return;
                    });
                co_return;
            });

            co_return;
        },
        "SyncGeneratorSum");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Sum: 1+2+3+4+5 = 15
    CHECK(sync_result.load() == 15);

    executor.shutdown();
}

TEST_CASE("TaskScope - AsyncGenerator produces correct sum") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> async_result{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                scope.spawn_async_producer(
                    channel,
                    [](TaskContext& inner_ctx) -> coro::AsyncGenerator<int> {
                        for (int i = 1; i <= 5; ++i) {
                            // Simulate async operation with small delay
                            auto val = co_await inner_ctx.spawn_io([i]() {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                                return i;
                            });
                            co_yield val;
                        }
                    });

                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        async_result.fetch_add(item);
                        co_return;
                    });
                co_return;
            });

            co_return;
        },
        "AsyncGeneratorSum");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Sum: 1+2+3+4+5 = 15
    CHECK(async_result.load() == 15);

    executor.shutdown();
}

TEST_CASE(
    "TaskScope - Sequential sync and async generators in same coroutine") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sync_result{0};
    std::atomic<int> async_result{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            // Test 1: Synchronous Generator
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                scope.spawn_producer(channel,
                                     [](TaskContext&) -> coro::Generator<int> {
                                         for (int i = 1; i <= 5; ++i) {
                                             co_yield i;
                                         }
                                     });

                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        sync_result.fetch_add(item);
                        co_return;
                    });
                co_return;
            });

            // Test 2: Async Generator (after sync generator completes)
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                scope.spawn_async_producer(
                    channel,
                    [](TaskContext& inner_ctx) -> coro::AsyncGenerator<int> {
                        for (int i = 1; i <= 5; ++i) {
                            // Simulate async operation
                            auto val = co_await inner_ctx.spawn_io([i]() {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                                return i;
                            });
                            co_yield val;
                        }
                    });

                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        async_result.fetch_add(item);
                        co_return;
                    });
                co_return;
            });

            co_return;
        },
        "SyncThenAsyncComparison");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Both should produce same sum: 1+2+3+4+5 = 15
    CHECK(sync_result.load() == 15);
    CHECK(async_result.load() == 15);

    executor.shutdown();
}

TEST_CASE("TaskScope - AsyncGenerator with complex async operations") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(10);

                // Producer with nested async operations
                scope.spawn_async_producer(
                    channel,
                    [](TaskContext& inner_ctx) -> coro::AsyncGenerator<int> {
                        for (int i = 1; i <= 5; ++i) {
                            // First async I/O
                            auto raw = co_await inner_ctx.spawn_io([i]() {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(2));
                                return i * 2;  // 2, 4, 6, 8, 10
                            });

                            // Second async I/O (processing)
                            auto processed =
                                co_await inner_ctx.spawn_io([raw]() {
                                    std::this_thread::sleep_for(
                                        std::chrono::milliseconds(2));
                                    return raw + 1;  // 3, 5, 7, 9, 11
                                });

                            co_yield processed;
                        }
                    });

                // Consumer
                scope.spawn_consumers(
                    channel, 1,
                    [&](TaskContext&, int item) -> coro::CoroTask<void> {
                        sum.fetch_add(item);
                        co_return;
                    });
                co_return;
            });

            co_return;
        },
        "ComplexAsyncOperations");

    scheduler.schedule(parent_task);
    parent_task->wait();

    // Sum: 3 + 5 + 7 + 9 + 11 = 35
    CHECK(sum.load() == 35);

    executor.shutdown();
}

TEST_CASE("TaskScope - AsyncGenerator with multiple async consumers") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> items_processed{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto channel = coro::make_channel<int>(30);

                // Async producer
                scope.spawn_async_producer(
                    channel,
                    [](TaskContext& inner_ctx) -> coro::AsyncGenerator<int> {
                        for (int i = 0; i < 20; ++i) {
                            co_await inner_ctx.spawn_io([i]() {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                                return i;
                            });
                            co_yield i;
                        }
                    });

                // Multiple async consumers (4 consumers)
                scope.spawn_consumers(channel, 4,
                                      [&](TaskContext& consumer_ctx,
                                          int) -> coro::CoroTask<void> {
                                          // Consumer also does async I/O
                                          co_await consumer_ctx.spawn_io([]() {
                                              std::this_thread::sleep_for(
                                                  std::chrono::milliseconds(2));
                                          });
                                          items_processed++;
                                          co_return;
                                      });
                co_return;
            });

            co_return;
        },
        "AsyncProducerMultipleAsyncConsumers");

    scheduler.schedule(parent_task);
    parent_task->wait();

    CHECK(items_processed.load() == 20);

    executor.shutdown();
}
