// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace dftracer::utils;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto DEADLINE = 20s;

coro::CoroTask<void> relay_until(coro::AsyncSemaphore& in,
                                 coro::AsyncSemaphore& out,
                                 std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        co_await in.acquire(1);
        out.release(1);
    }
    out.release(1);
}

}  // namespace

TEST_SUITE("PoolScheduling") {
    TEST_CASE("children spawned on one worker reach the other workers") {
        // Every child spins until all have started, so they need a worker
        // each.
        constexpr std::size_t THREADS = dftu_utils_test::pool_width(4);
        Runtime rt(THREADS);
        std::atomic<std::size_t> started{0};
        std::atomic<bool> all_met{true};
        dftu_utils_test::ok(rt.block_on(
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                for (std::size_t i = 0; i < THREADS; ++i) {
                    scope.spawn([&]() -> coro::CoroTask<void> {
                        started.fetch_add(1, std::memory_order_acq_rel);
                        const auto until = Clock::now() + DEADLINE;
                        while (started.load(std::memory_order_acquire) <
                               THREADS) {
                            if (Clock::now() > until) {
                                all_met.store(false);
                                break;
                            }
                            std::this_thread::yield();
                        }
                        co_return;
                    });
                }
                co_return;
            })));
        CHECK(all_met.load());
    }

    TEST_CASE("a task woken before a long computation does not wait for it") {
        constexpr auto COMPUTE = 300ms;
        constexpr auto MAX_DELAY = 100ms;
        Runtime rt(4);
        std::atomic<std::int64_t> woken_at{0};
        coro::AsyncSemaphore gate(1);
        REQUIRE(gate.try_acquire(1));
        dftu_utils_test::ok(rt.block_on(
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn([&]() -> coro::CoroTask<void> {
                    co_await gate.acquire(1);
                    woken_at.store(Clock::now().time_since_epoch().count());
                });
                scope.spawn([&]() -> coro::CoroTask<void> {
                    std::this_thread::sleep_for(50ms);
                    const auto start = Clock::now();
                    gate.release(1);
                    while (Clock::now() - start < COMPUTE) {
                    }
                    const auto woken =
                        Clock::time_point(Clock::duration(woken_at.load()));
                    CHECK(woken_at.load() != 0);
                    CHECK(woken - start < MAX_DELAY);
                    co_return;
                });
                co_return;
            })));
    }

    TEST_CASE("two tasks that wake each other do not starve a queued task") {
        Runtime rt(1);
        std::atomic<bool> stop{false};
        coro::AsyncSemaphore first(1);
        coro::AsyncSemaphore second(1);
        REQUIRE(second.try_acquire(1));
        dftu_utils_test::ok(rt.block_on(
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn([&]() -> coro::CoroTask<void> {
                    co_await coro::yield();
                    stop.store(true, std::memory_order_release);
                });
                scope.spawn(relay_until(first, second, stop));
                scope.spawn(relay_until(second, first, stop));
                co_return;
            })));
        CHECK(stop.load());
    }

    TEST_CASE("a task sent from outside runs behind a worker that keeps busy") {
        Runtime rt(1);
        std::atomic<bool> stop{false};
        auto spinner = [&]() -> coro::CoroTask<void> {
            while (!stop.load(std::memory_order_acquire)) {
                co_await coro::yield();
            }
        };
        auto setter = [&]() -> coro::CoroTask<void> {
            stop.store(true, std::memory_order_release);
            co_return;
        };
        TaskHandle spin = rt.submit(spinner(), "Spinner");
        std::this_thread::sleep_for(10ms);
        TaskHandle set = rt.submit(setter(), "Setter");
        dftu_utils_test::ok(set.wait());
        dftu_utils_test::ok(spin.wait());
        CHECK(stop.load());
    }

    TEST_CASE("a wake after the pool goes idle does not wait out a timer") {
        // The idle park waits 256 ms. A lost wake would cost that much.
        constexpr int ROUNDS = 51;
        constexpr auto SLOW = 200ms;
        constexpr std::size_t MAX_SLOW_ROUNDS = 2;
        Runtime rt(4);
        auto tiny = []() -> coro::CoroTask<void> { co_return; };
        dstd::Vector<std::chrono::nanoseconds> latencies;
        for (int round = 0; round < ROUNDS; ++round) {
            std::this_thread::sleep_for(1ms);
            const auto start = Clock::now();
            dftu_utils_test::ok(rt.submit(tiny(), "Tiny").wait());
            latencies.push_back(Clock::now() - start);
        }
        // A round or two can stall on a loaded machine. Lost wakes repeat.
        const auto slow_rounds =
            std::count_if(latencies.begin(), latencies.end(),
                          [&](auto latency) { return latency >= SLOW; });
        CHECK(static_cast<std::size_t>(slow_rounds) <= MAX_SLOW_ROUNDS);
    }

    TEST_CASE("a burst from many outside threads all runs") {
        constexpr std::size_t SENDERS = 8;
        constexpr std::size_t TASKS = 2000;
        Runtime rt(4);
        std::atomic<std::size_t> ran{0};
        auto tiny = [&]() -> coro::CoroTask<void> {
            ran.fetch_add(1, std::memory_order_relaxed);
            co_return;
        };
        dstd::Vector<dstd::Thread> senders;
        for (std::size_t s = 0; s < SENDERS; ++s) {
            senders.emplace_back([&] {
                dstd::Vector<TaskHandle> handles;
                for (std::size_t i = 0; i < TASKS; ++i) {
                    handles.push_back(rt.submit(tiny(), "Tiny"));
                }
                for (auto& handle : handles) dftu_utils_test::ok(handle.wait());
            });
        }
        for (auto& sender : senders) sender.join();
        CHECK(ran.load() == SENDERS * TASKS);
    }
}
