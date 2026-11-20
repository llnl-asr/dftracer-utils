// SPDX-License-Identifier: MIT
// A thread that cannot be created (the RLIMIT_NPROC or pids.max limit) must
// leave the runtime working with the threads it has.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/thread_probe.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace dftracer::utils;

namespace {

coro::CoroTask<void> bump(std::atomic<int>* done) {
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

coro::CoroTask<void> gated(std::atomic<bool>* go, std::atomic<int>* done) {
    while (!go->load(std::memory_order_acquire)) std::this_thread::yield();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Blocks its worker until `flag` is set or 5 s pass, and records what it saw.
coro::CoroTask<void> block_until(std::atomic<bool>* flag,
                                 std::atomic<bool>* seen) {
    {
        coro::BlockingRegion region;
        const auto until = dstd::steady_now() + std::chrono::seconds(5);
        while (!flag->load(std::memory_order_acquire) &&
               dstd::steady_now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    seen->store(flag->load(std::memory_order_acquire),
                std::memory_order_release);
    co_return;
}

coro::CoroTask<void> raise(std::atomic<bool>* flag) {
    flag->store(true, std::memory_order_release);
    co_return;
}

struct ProbeOff {
    ~ProbeOff() { dstd::detail::fail_thread_creates(0); }
};

}  // namespace

TEST_SUITE("thread_start_failure") {
    TEST_CASE("a worker that cannot start leaves the pool as it was") {
        ProbeOff off;
        Runtime rt(4);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        exec->retire_worker();
        exec->retire_worker();
        exec->retire_worker();
        REQUIRE(exec->live_workers() == 1);

        dstd::detail::fail_thread_creates(1);
        CHECK_FALSE(exec->add_worker());
        CHECK(exec->live_workers() == 1);
        // The pool waits before it tries again, so a limit is not hit in a
        // loop.
        CHECK_FALSE(exec->add_worker());
        CHECK(exec->live_workers() == 1);

        std::atomic<int> done{0};
        const int n = 5000;
        for (int i = 0; i < n; ++i) rt.submit(bump(&done));
        rt.wait_all();
        CHECK(done.load() == n);

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK(exec->add_worker());
        CHECK(exec->live_workers() == 2);
    }

    TEST_CASE("an elastic pool that cannot grow drains its backlog") {
        ProbeOff off;
        ExecutorConfig config;
        config.thread_count = 4;
        config.min_workers = 1;
        Runtime rt(config, false);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        REQUIRE(exec->live_workers() == 1);

        dstd::detail::fail_thread_creates(1000000);
        std::atomic<bool> go{false};
        std::atomic<int> done{0};
        const int n = 64;
        for (int i = 0; i < n; ++i) rt.submit(gated(&go, &done));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(exec->live_workers() == 1);
        go.store(true, std::memory_order_release);
        rt.wait_all();
        CHECK(done.load() == n);
    }

    TEST_CASE("a blocked worker gets its replacement once threads start") {
        ProbeOff off;
        Runtime rt(1);
        std::atomic<bool> flag{false};
        std::atomic<bool> seen{false};
        dstd::detail::fail_thread_creates(1);
        rt.submit(block_until(&flag, &seen));
        rt.submit(raise(&flag));
        rt.wait_all();
        CHECK(seen.load());
    }

    TEST_CASE("a timer service whose thread cannot start reports it") {
        ProbeOff off;
        coro::TimerService timers;
        dstd::detail::fail_thread_creates(1);
        CHECK_FALSE(timers.start().has_value());
        CHECK(timers.start().has_value());
        timers.stop();
    }
}
