#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/pipeline/thread_pool_executor.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <vector>

using namespace dftracer::utils;

// ============================================================================
// Executor::current() TLS
// ============================================================================

TEST_CASE("Yield - Executor::current() is nullptr outside worker") {
    CHECK(Executor::current() == nullptr);
}

TEST_CASE("Yield - Executor::current() is set inside worker task") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 1});
    Scheduler scheduler(&executor);

    std::atomic<Executor*> observed{nullptr};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            observed.store(Executor::current());
            co_return;
        },
        "CheckCurrent");

    scheduler.schedule(task);
    task->wait();
    CHECK(observed.load() == &executor);
    executor.shutdown();
}

// ============================================================================
// maybe_yield() -- conditional yield based on timeslice
// ============================================================================

TEST_CASE("Yield - maybe_yield() does not hang with executor") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            for (int i = 0; i < 100; ++i) {
                counter.fetch_add(1);
                co_await coro::maybe_yield();
            }
            co_return;
        },
        "MaybeYieldLoop");

    scheduler.schedule(task);
    task->wait();
    CHECK(counter.load() == 100);
    executor.shutdown();
}

// ============================================================================
// yield() -- unconditional yield
// ============================================================================

TEST_CASE("Yield - yield() does not hang with executor") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            for (int i = 0; i < 10; ++i) {
                counter.fetch_add(1);
                co_await coro::yield();
            }
            co_return;
        },
        "YieldLoop");

    scheduler.schedule(task);
    task->wait();
    CHECK(counter.load() == 10);
    executor.shutdown();
}

// ============================================================================
// Timeslice behavior
// ============================================================================

TEST_CASE("Yield - timeslice functions are thread-local") {
    // On the main thread (not a worker), timeslice_exceeded should
    // return false right after reset.
    coro::reset_timeslice();
    CHECK(coro::timeslice_exceeded() == false);
}

TEST_CASE("Yield - timeslice duration 0 disables yielding") {
    coro::set_timeslice_duration(std::chrono::microseconds{0});
    CHECK(coro::timeslice_exceeded() == false);
    // Restore default
    coro::set_timeslice_duration(coro::DEFAULT_TIMESLICE);
}

// ============================================================================
// Concurrent yield with multiple coroutines
// ============================================================================

TEST_CASE("Yield - Multiple coroutines yielding concurrently") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    constexpr int NUM_TASKS = 20;
    constexpr int ITERS_PER_TASK = 50;
    std::atomic<int> total{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    for (int t = 0; t < NUM_TASKS; ++t) {
                        scope.spawn(
                            [&total](CoroScope&) -> coro::CoroTask<void> {
                                for (int i = 0; i < ITERS_PER_TASK; ++i) {
                                    total.fetch_add(1);
                                    co_await coro::maybe_yield();
                                }
                                co_return;
                            });
                    }
                    co_return;
                });
            co_return;
        },
        "ConcurrentYield");

    scheduler.schedule(task);
    task->wait();
    CHECK(total.load() == NUM_TASKS * ITERS_PER_TASK);
    executor.shutdown();
}

TEST_CASE("Yield - Forced yield re-enqueues on executor") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    // Two tasks that yield() back and forth.  Each increments a counter.
    // If yield() didn't re-enqueue, one would starve.
    constexpr int ITERS = 20;
    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    scope.spawn([&a_count](CoroScope&) -> coro::CoroTask<void> {
                        for (int i = 0; i < ITERS; ++i) {
                            a_count.fetch_add(1);
                            co_await coro::yield();
                        }
                        co_return;
                    });
                    scope.spawn([&b_count](CoroScope&) -> coro::CoroTask<void> {
                        for (int i = 0; i < ITERS; ++i) {
                            b_count.fetch_add(1);
                            co_await coro::yield();
                        }
                        co_return;
                    });
                    co_return;
                });
            co_return;
        },
        "ForcedYieldPingPong");

    scheduler.schedule(task);
    task->wait();
    CHECK(a_count.load() == ITERS);
    CHECK(b_count.load() == ITERS);
    executor.shutdown();
}

TEST_CASE("CoroTask::get on a pool worker sees the finished task's result") {
    // The inner task finishes on whichever worker completes the last part of
    // its when_all, so the worker blocked in get() learns of it only through
    // the task's own completion signal.
    auto part = [](int base) -> coro::CoroTask<std::vector<int>> {
        std::vector<int> v;
        for (int i = 0; i < 512; ++i) v.push_back(base + i);
        co_await coro::yield();
        co_return v;
    };
    auto inner = [part]() -> coro::CoroTask<std::vector<int>> {
        std::vector<coro::CoroTask<std::vector<int>>> parts;
        for (int k = 0; k < 8; ++k) parts.push_back(part(k * 512));
        std::vector<std::vector<int>> got =
            co_await coro::when_all(std::move(parts));
        std::vector<int> all;
        for (auto& g : got) all.insert(all.end(), g.begin(), g.end());
        co_return all;
    };
    auto outer = [inner]() -> coro::CoroTask<long long> {
        std::vector<int> v = inner().get();
        long long sum = 0;
        for (int x : v) sum += x;
        co_return sum;
    };
    for (int round = 0; round < 64; ++round)
        CHECK(dftracer::utils::default_runtime().submit(outer()).get() ==
              4096LL * 4095 / 2);
}
