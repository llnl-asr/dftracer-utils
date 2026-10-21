#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace dftracer::utils;
using namespace std::chrono_literals;

namespace {

ExecutorConfig one_thread() {
    ExecutorConfig config;
    config.num_threads = 1;
    config.deadlock_timeout = 1s;
    return config;
}

}  // namespace

TEST_SUITE("ExecutorResponsiveness") {
    TEST_CASE("a long task that keeps resuming is not a deadlock") {
        Runtime rt(one_thread(), false);
        std::atomic<bool> stop{false};
        auto busy = [&]() -> coro::CoroTask<void> {
            while (!stop.load(std::memory_order_relaxed)) {
                co_await coro::yield();
            }
        };
        auto handle = rt.submit(busy(), "Busy");
        // The check samples progress, so it needs one earlier look.
        std::this_thread::sleep_for(100ms);
        CHECK(rt.is_responsive());
        std::this_thread::sleep_for(1400ms);
        CHECK(rt.is_responsive());
        stop.store(true);
        handle.wait();
        rt.shutdown();
    }

    TEST_CASE("a task that blocks its only thread is a deadlock") {
        Runtime rt(one_thread(), false);
        std::atomic<bool> stop{false};
        auto stuck = [&]() -> coro::CoroTask<void> {
            while (!stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(10ms);
            }
            co_return;
        };
        auto handle = rt.submit(stuck(), "Stuck");
        std::this_thread::sleep_for(100ms);
        CHECK(rt.is_responsive());
        std::this_thread::sleep_for(1400ms);
        CHECK_FALSE(rt.is_responsive());
        stop.store(true);
        handle.wait();
        rt.shutdown();
    }
}
