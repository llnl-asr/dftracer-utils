// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace dftracer::utils;

namespace {

constexpr std::size_t POOL_THREADS = 4;

coro::CoroTask<void> fork_join_task(Runtime* rt, std::atomic<int>* done) {
    dftu_utils_test::ok(rt->parallel_for(8, 1, [](std::int64_t, std::int64_t) {
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }));
    done->fetch_add(1);
    co_return;
}

}  // namespace

TEST_CASE("fork-join from workers does not grow the pool") {
    Runtime rt(ExecutorConfig{.thread_count = POOL_THREADS}, false);
    auto& pool = static_cast<ThreadPoolExecutor&>(*rt.executor());
    constexpr int TASKS = 200;
    std::atomic<int> done{0};
    dstd::Vector<TaskHandle> handles;
    for (int i = 0; i < TASKS; ++i)
        handles.push_back(rt.submit(fork_join_task(&rt, &done), "fork-join"));
    std::size_t peak = pool.live_workers();
    while (done.load() < TASKS) {
        peak = std::max(peak, pool.live_workers());
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    for (auto& h : handles) dftu_utils_test::ok(h.wait());
    CHECK(peak <= POOL_THREADS + 2);
    rt.shutdown();
}

TEST_CASE("parallel_for covers every index once under contention") {
    Runtime rt(4);
    constexpr std::int64_t N = 50000;
    constexpr int CALLERS = 8;
    dstd::Vector<dstd::Vector<std::int32_t>> hits(CALLERS);
    for (auto& h : hits) h.assign(N, 0);
    dstd::Vector<dstd::Thread> callers;
    for (int c = 0; c < CALLERS; ++c)
        callers.emplace_back([&, c] {
            for (int round = 0; round < 20; ++round)
                dftu_utils_test::ok(rt.parallel_for(
                    N, 97, [&](std::int64_t begin, std::int64_t end) {
                        for (std::int64_t i = begin; i < end; ++i) hits[c][i]++;
                    }));
        });
    for (auto& t : callers) t.join();
    for (const auto& h : hits)
        for (std::int64_t i = 0; i < N; ++i) REQUIRE(h[i] == 20);
}

TEST_CASE("parallel_reduce equals the serial result under contention") {
    Runtime rt(4);
    constexpr std::int64_t N = 100000;
    std::int64_t expected = 0;
    for (std::int64_t i = 0; i < N; ++i) expected += i;
    std::atomic<int> wrong{0};
    dstd::Vector<dstd::Thread> callers;
    for (int c = 0; c < 8; ++c)
        callers.emplace_back([&] {
            for (int round = 0; round < 20; ++round) {
                auto got = rt.parallel_reduce(
                    N, 101, std::int64_t{0},
                    [](std::int64_t b, std::int64_t e) {
                        std::int64_t s = 0;
                        for (std::int64_t i = b; i < e; ++i) s += i;
                        return s;
                    },
                    [](std::int64_t a, std::int64_t b) { return a + b; });
                if (!got || *got != expected) wrong.fetch_add(1);
            }
        });
    for (auto& t : callers) t.join();
    CHECK(wrong.load() == 0);
}

TEST_CASE("a nested parallel_for stays inline on the calling chunk") {
    Runtime rt(4);
    std::atomic<int> inner_calls{0};
    std::atomic<int> moved{0};
    dftu_utils_test::ok(rt.parallel_for(16, 1, [&](std::int64_t, std::int64_t) {
        const auto outer = std::this_thread::get_id();
        dftu_utils_test::ok(
            rt.parallel_for(64, 1, [&](std::int64_t, std::int64_t) {
                inner_calls.fetch_add(1);
                if (std::this_thread::get_id() != outer) moved.fetch_add(1);
            }));
    }));
    CHECK(inner_calls.load() == 16);
    CHECK(moved.load() == 0);
}
