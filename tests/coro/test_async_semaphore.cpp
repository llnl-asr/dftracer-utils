// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace dftracer::utils;

TEST_SUITE("CoroSemaphore") {
    TEST_CASE("never exceeds capacity under contention") {
        coro::CoroSemaphore sem(100);
        std::atomic<std::int64_t> in_use{0};
        std::atomic<std::int64_t> peak{0};
        const int n = 64;

        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            dstd::Vector<coro::CoroTask<void>> tasks;
            for (int i = 0; i < n; ++i) {
                // Sizes 10..40 so several fit at once but not all.
                std::uint64_t need = 10 + (i % 4) * 10;
                tasks.push_back(
                    [](coro::CoroSemaphore& s, std::uint64_t bytes,
                       std::atomic<std::int64_t>& cur,
                       std::atomic<std::int64_t>& hi) -> coro::CoroTask<void> {
                        co_await s.acquire(bytes);
                        std::int64_t now = cur.fetch_add((std::int64_t)bytes) +
                                           (std::int64_t)bytes;
                        std::int64_t seen = hi.load();
                        while (now > seen &&
                               !hi.compare_exchange_weak(seen, now)) {
                        }
                        for (int k = 0; k < 4; ++k) co_await coro::yield();
                        cur.fetch_sub((std::int64_t)bytes);
                        s.release(bytes);
                        co_return;
                    }(sem, need, in_use, peak));
            }
            co_await coro::when_all(std::move(tasks));
            co_return;
        });

        CHECK(peak.load() <= 100);
        CHECK(peak.load() > 0);
        CHECK(in_use.load() == 0);
    }

    TEST_CASE("an over-capacity request still completes") {
        coro::CoroSemaphore sem(50);
        std::atomic<bool> done{false};

        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            // 200 > capacity 50: clamps to capacity and runs once idle.
            co_await sem.acquire(200);
            done.store(true);
            sem.release(200);
            co_return;
        });

        CHECK(done.load());
    }

    TEST_CASE("all acquirers eventually make progress") {
        coro::CoroSemaphore sem(16);
        std::atomic<int> completed{0};
        const int n = 100;

        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            dstd::Vector<coro::CoroTask<void>> tasks;
            for (int i = 0; i < n; ++i) {
                tasks.push_back(
                    [](coro::CoroSemaphore& s,
                       std::atomic<int>& c) -> coro::CoroTask<void> {
                        co_await s.acquire(8);
                        co_await coro::yield();
                        c.fetch_add(1);
                        s.release(8);
                        co_return;
                    }(sem, completed));
            }
            co_await coro::when_all(std::move(tasks));
            co_return;
        });

        CHECK(completed.load() == n);
    }

    // A waiter parked on acquire() must resume when release() is called from a
    // thread with no Executor::current() (a plain dstd::Thread). Bounded
    // wait: a regression manifests as a hang, so time out and fail rather than
    // block.
    TEST_CASE("release() from an off-executor thread wakes a parked waiter") {
        coro::CoroSemaphore sem(10);
        std::atomic<bool> waiter_done{false};
        std::atomic<bool> finished{false};

        dstd::Thread worker([&] {
            dftu_utils_test::run_coro([&](coro::Scope&)
                                          -> coro::CoroTask<void> {
                co_await sem.acquire(10);  // take all capacity
                dstd::Thread releaser([&sem] {
                    // No Executor::current() here.
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    sem.release(10);
                });
                co_await sem.acquire(10);  // parks until the foreign release
                waiter_done.store(true);
                sem.release(10);
                releaser.join();
                co_return;
            });
            finished.store(true);
        });

        for (int i = 0; i < 200 && !finished.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));

        if (!finished.load()) {
            worker.detach();
            FAIL(
                "off-executor release() did not wake the parked waiter (hang)");
        } else {
            worker.join();
            CHECK(waiter_done.load());
        }
    }
}
