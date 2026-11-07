// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/watchdog.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

using namespace dftracer::utils;
using namespace std::chrono_literals;

namespace {

ExecutorConfig one_thread() {
    ExecutorConfig config;
    config.thread_count = 1;
    config.deadlock_timeout = 1s;
    return config;
}

bool wait_for(const std::atomic<bool>& flag) {
    for (int i = 0; i < 1000 && !flag.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    return flag.load();
}

}  // namespace

TEST_SUITE("Watchdog") {
    TEST_CASE("a task past the default timeout fires the timeout callback") {
        Watchdog dog(10ms, 0ms, 20ms, 0ms);
        std::atomic<bool> fired{false};
        std::mutex reason_mutex;
        dstd::String reason;
        dog.set_timeout_callback([&](dstd::StringView text) {
            std::lock_guard<std::mutex> lock(reason_mutex);
            reason.assign(text.data(), text.size());
            fired.store(true);
        });
        dog.start();
        dog.register_task_start(7, "slow");
        CHECK(wait_for(fired));
        dog.stop();
        std::lock_guard<std::mutex> lock(reason_mutex);
        CHECK(reason.find("Task 'slow' timed out") != dstd::String::npos);
    }

    TEST_CASE("a task that ended before its timeout fires nothing") {
        Watchdog dog(10ms, 0ms, 50ms, 0ms);
        std::atomic<bool> fired{false};
        dog.set_timeout_callback([&](dstd::StringView) { fired.store(true); });
        dog.start();
        dog.register_task_start(7, "quick");
        dog.unregister_task(7);
        std::this_thread::sleep_for(150ms);
        dog.stop();
        CHECK_FALSE(fired.load());
    }

    TEST_CASE("a long task warns once with its name") {
        Watchdog dog(10ms, 0ms, 0ms, 20ms);
        std::atomic<int> warnings{0};
        std::atomic<bool> named{false};
        dog.set_warning_callback([&](dstd::StringView name, std::int64_t) {
            if (name == "long") named.store(true);
            warnings.fetch_add(1);
        });
        dog.start();
        dog.register_task_start(7, "long");
        std::this_thread::sleep_for(150ms);
        dog.stop();
        CHECK(warnings.load() == 1);
        CHECK(named.load());
    }

    TEST_CASE("the global timeout fires the timeout callback") {
        Watchdog dog(10ms, 20ms, 0ms, 0ms);
        std::atomic<bool> fired{false};
        dog.set_timeout_callback([&](dstd::StringView) { fired.store(true); });
        dog.start();
        dog.mark_execution_start();
        CHECK(wait_for(fired));
        dog.stop();
    }

    TEST_CASE("a zero global timeout fires nothing") {
        Watchdog dog(10ms, 0ms, 0ms, 0ms);
        std::atomic<bool> fired{false};
        dog.set_timeout_callback([&](dstd::StringView) { fired.store(true); });
        dog.start();
        dog.mark_execution_start();
        std::this_thread::sleep_for(100ms);
        dog.stop();
        CHECK_FALSE(fired.load());
    }
}

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
