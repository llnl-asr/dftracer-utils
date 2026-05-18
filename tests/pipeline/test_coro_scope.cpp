#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>

using namespace dftracer::utils;

TEST_CASE("CoroScope - Simple spawn") {
    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    for (int i = 0; i < 3; ++i) {
                        scope.spawn(
                            [&counter](CoroScope&) -> coro::CoroTask<void> {
                                ++counter;
                                co_return;
                            });
                    }
                    co_return;
                });
            co_return;
        },
        "SimpleSpawn");

    scheduler.schedule(task);
    task->wait();
    CHECK(counter.load() == 3);
    executor.shutdown();
}

TEST_CASE("CoroScope - Many spawns") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    constexpr int N = 100;
    std::atomic<int> counter{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    for (int i = 0; i < N; ++i) {
                        scope.spawn(
                            [&counter](CoroScope&) -> coro::CoroTask<void> {
                                ++counter;
                                co_return;
                            });
                    }
                    co_return;
                });
            CHECK(counter.load() == N);
            co_return;
        },
        "ManySpawns");

    scheduler.schedule(task);
    task->wait();
    CHECK(counter.load() == N);
    executor.shutdown();
}

TEST_CASE("CoroScope - Producer-consumer with channel (reference)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            coro::Channel<int> channel(16);

            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    // Producer: generate 1..10
                    scope.spawn_producer(
                        channel, [](CoroScope&) -> coro::Generator<int> {
                            for (int i = 1; i <= 10; ++i) {
                                co_yield i;
                            }
                        });

                    // 2 consumers
                    scope.spawn_consumers(
                        channel, 2,
                        [&sum](CoroScope&, int val) -> coro::CoroTask<void> {
                            sum.fetch_add(val, std::memory_order_relaxed);
                            co_return;
                        });

                    co_return;
                });

            CHECK(sum.load() == 55);  // 1+2+...+10
            co_return;
        },
        "ProducerConsumerRef");

    scheduler.schedule(task);
    task->wait();
    CHECK(sum.load() == 55);
    executor.shutdown();
}

TEST_CASE("CoroScope - Producer-consumer with channel (shared_ptr)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto channel = coro::make_channel<int>(16);

            co_await ctx.coro_scope(
                [&sum, &channel](CoroScope& scope) -> coro::CoroTask<void> {
                    scope.spawn_producer(
                        channel, [](CoroScope&) -> coro::Generator<int> {
                            for (int i = 1; i <= 10; ++i) {
                                co_yield i;
                            }
                        });

                    scope.spawn_consumers(
                        channel, 2,
                        [&sum](CoroScope&, int val) -> coro::CoroTask<void> {
                            sum.fetch_add(val, std::memory_order_relaxed);
                            co_return;
                        });

                    co_return;
                });

            CHECK(sum.load() == 55);
            co_return;
        },
        "ProducerConsumerPtr");

    scheduler.schedule(task);
    task->wait();
    CHECK(sum.load() == 55);
    executor.shutdown();
}

TEST_CASE("CoroScope - Transform pipeline") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto input = coro::make_channel<int>(16);
            auto output = coro::make_channel<int>(16);

            co_await ctx.coro_scope(
                [&sum, &input,
                 &output](CoroScope& scope) -> coro::CoroTask<void> {
                    // Producer: 1..5
                    scope.spawn_producer(
                        input, [](CoroScope&) -> coro::Generator<int> {
                            for (int i = 1; i <= 5; ++i) {
                                co_yield i;
                            }
                        });

                    // Transform: double each value
                    scope.spawn_transforms(
                        input, output, 2,
                        [](CoroScope&, int val) -> coro::CoroTask<int> {
                            co_return val * 2;
                        });

                    // Consumer: sum results
                    scope.spawn_consumers(
                        output, 1,
                        [&sum](CoroScope&, int val) -> coro::CoroTask<void> {
                            sum.fetch_add(val, std::memory_order_relaxed);
                            co_return;
                        });

                    co_return;
                });

            // (1+2+3+4+5)*2 = 30
            CHECK(sum.load() == 30);
            co_return;
        },
        "TransformPipeline");

    scheduler.schedule(task);
    task->wait();
    CHECK(sum.load() == 30);
    executor.shutdown();
}

TEST_CASE("CoroScope - Manual join") {
    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            CoroScope scope(ctx.get_executor());
            for (int i = 0; i < 5; ++i) {
                scope.spawn([&counter](CoroScope&) -> coro::CoroTask<void> {
                    ++counter;
                    co_return;
                });
            }
            co_await scope.join();
            CHECK(counter.load() == 5);
            co_return;
        },
        "ManualJoin");

    scheduler.schedule(task);
    task->wait();
    CHECK(counter.load() == 5);
    executor.shutdown();
}

TEST_CASE("CoroScope - spawn_producers (N producers, shared_ptr)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto channel = coro::make_channel<int>(32);

            co_await ctx.coro_scope(
                [&sum, &channel](CoroScope& scope) -> coro::CoroTask<void> {
                    // 3 producers, each sends its index
                    scope.spawn_producers(
                        channel, 3,
                        [&channel](CoroScope&,
                                   std::size_t idx) -> coro::CoroTask<void> {
                            int val = static_cast<int>(idx + 1);
                            co_await channel->send(val);
                            co_return;
                        });

                    // 1 consumer sums everything
                    scope.spawn_consumers(
                        channel, 1,
                        [&sum](CoroScope&, int val) -> coro::CoroTask<void> {
                            sum.fetch_add(val, std::memory_order_relaxed);
                            co_return;
                        });

                    co_return;
                });

            // 1 + 2 + 3 = 6
            CHECK(sum.load() == 6);
            co_return;
        },
        "NProducers");

    scheduler.schedule(task);
    task->wait();
    CHECK(sum.load() == 6);
    executor.shutdown();
}

TEST_CASE("CoroScope - spawn works for compute and I/O tasks") {
    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<int> result{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    scope.spawn(
                        [&result](CoroScope& /*s*/) -> coro::CoroTask<void> {
                            auto val = 42;
                            result.store(val, std::memory_order_relaxed);
                            co_return;
                        });
                    co_return;
                });

            CHECK(result.load() == 42);
            co_return;
        },
        "SpawnIOAccess");

    scheduler.schedule(task);
    task->wait();
    CHECK(result.load() == 42);
    executor.shutdown();
}

TEST_CASE("CoroScope - Empty scope (no spawns)") {
    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    bool completed = false;

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope([](CoroScope&) -> coro::CoroTask<void> {
                // No spawns -- join should return immediately
                co_return;
            });
            completed = true;
            co_return;
        },
        "EmptyScope");

    scheduler.schedule(task);
    task->wait();
    CHECK(completed);
    executor.shutdown();
}

TEST_CASE("CoroScope - Typed spawn with SpawnFuture") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> result{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    auto future =
                        scope.spawn([](CoroScope&) -> coro::CoroTask<int> {
                            co_return 42;
                        });
                    int val = co_await std::move(future);
                    result.store(val, std::memory_order_relaxed);
                    co_return;
                });
            co_return;
        },
        "TypedSpawn");

    scheduler.schedule(task);
    task->wait();
    CHECK(result.load() == 42);
    executor.shutdown();
}

TEST_CASE("CoroScope - Multiple typed spawns") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> result{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope([&](CoroScope& scope)
                                        -> coro::CoroTask<void> {
                auto f1 = scope.spawn(
                    [](CoroScope&) -> coro::CoroTask<int> { co_return 10; });
                auto f2 = scope.spawn(
                    [](CoroScope&) -> coro::CoroTask<int> { co_return 20; });
                auto f3 = scope.spawn(
                    [](CoroScope&) -> coro::CoroTask<int> { co_return 12; });
                int v1 = co_await std::move(f1);
                int v2 = co_await std::move(f2);
                int v3 = co_await std::move(f3);
                result.store(v1 + v2 + v3, std::memory_order_relaxed);
                co_return;
            });
            co_return;
        },
        "MultiTypedSpawn");

    scheduler.schedule(task);
    task->wait();
    CHECK(result.load() == 42);
    executor.shutdown();
}
