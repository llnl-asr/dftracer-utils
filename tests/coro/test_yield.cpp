// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <chrono>

using namespace dftracer::utils;

// ============================================================================
// Executor::current() TLS
// ============================================================================

TEST_CASE("Yield - Executor::current() is nullptr outside worker") {
    CHECK(coro::Executor::current() == nullptr);
}

TEST_CASE("Yield - Executor::current() is set inside worker task") {
    Runtime rt(1);

    std::atomic<coro::Executor*> observed{nullptr};

    auto body = [&]() -> coro::CoroTask<void> {
        observed.store(coro::Executor::current());
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(observed.load() == rt.executor());
}

// ============================================================================
// maybe_yield() -- conditional yield based on timeslice
// ============================================================================

TEST_CASE("Yield - maybe_yield() does not hang with executor") {
    Runtime rt(2);

    std::atomic<int> counter{0};

    auto body = [&]() -> coro::CoroTask<void> {
        for (int i = 0; i < 100; ++i) {
            counter.fetch_add(1);
            co_await coro::maybe_yield();
        }
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(counter.load() == 100);
}

// ============================================================================
// yield() -- unconditional yield
// ============================================================================

TEST_CASE("Yield - yield() does not hang with executor") {
    Runtime rt(2);

    std::atomic<int> counter{0};

    auto body = [&]() -> coro::CoroTask<void> {
        for (int i = 0; i < 10; ++i) {
            counter.fetch_add(1);
            co_await coro::yield();
        }
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(counter.load() == 10);
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
    Runtime rt(4);

    constexpr int NUM_TASKS = 20;
    constexpr int ITERS_PER_TASK = 50;
    std::atomic<int> total{0};

    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (int t = 0; t < NUM_TASKS; ++t) {
                scope.spawn([&total]() -> coro::CoroTask<void> {
                    for (int i = 0; i < ITERS_PER_TASK; ++i) {
                        total.fetch_add(1);
                        co_await coro::maybe_yield();
                    }
                    co_return;
                });
            }
            co_return;
        })));
    CHECK(total.load() == NUM_TASKS * ITERS_PER_TASK);
}

TEST_CASE("Yield - Forced yield re-enqueues on executor") {
    Runtime rt(2);

    // Two tasks that yield() back and forth.  Each increments a counter.
    // If yield() didn't re-enqueue, one would starve.
    constexpr int ITERS = 20;
    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};

    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            scope.spawn([&a_count]() -> coro::CoroTask<void> {
                for (int i = 0; i < ITERS; ++i) {
                    a_count.fetch_add(1);
                    co_await coro::yield();
                }
                co_return;
            });
            scope.spawn([&b_count]() -> coro::CoroTask<void> {
                for (int i = 0; i < ITERS; ++i) {
                    b_count.fetch_add(1);
                    co_await coro::yield();
                }
                co_return;
            });
            co_return;
        })));
    CHECK(a_count.load() == ITERS);
    CHECK(b_count.load() == ITERS);
}

TEST_CASE("CoroTask::get on a pool worker sees the finished task's result") {
    // The inner task finishes on whichever worker completes the last part of
    // its when_all, so the worker blocked in get() learns of it only through
    // the task's own completion signal.
    auto part = [](int base) -> coro::CoroTask<dstd::Vector<int>> {
        dstd::Vector<int> v;
        for (int i = 0; i < 512; ++i) v.push_back(base + i);
        co_await coro::yield();
        co_return v;
    };
    auto inner = [part]() -> coro::CoroTask<dstd::Vector<int>> {
        dstd::Vector<coro::CoroTask<dstd::Vector<int>>> parts;
        for (int k = 0; k < 8; ++k) parts.push_back(part(k * 512));
        auto got = co_await coro::when_all(std::move(parts));
        dstd::Vector<int> all;
        for (auto& g : got) all.insert(all.end(), g.begin(), g.end());
        co_return all;
    };
    auto outer = [inner]() -> coro::CoroTask<long long> {
        dstd::Vector<int> v = dftu_utils_test::ok(inner().get());
        long long sum = 0;
        for (int x : v) sum += x;
        co_return sum;
    };
    for (int round = 0; round < 64; ++round)
        CHECK(dftu_utils_test::ok(
                  dftracer::utils::default_runtime().submit(outer()).get()) ==
              4096LL * 4095 / 2);
}
