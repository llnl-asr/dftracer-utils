#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace std::chrono_literals;

namespace {

std::int64_t context_switches() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::int64_t>(usage.ru_nvcsw) + usage.ru_nivcsw;
}

// CPU time of the process, user plus system.
std::chrono::microseconds cpu_time() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto us = [](const timeval& tv) {
        return static_cast<std::int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
    };
    return std::chrono::microseconds(us(usage.ru_utime) + us(usage.ru_stime));
}

}  // namespace

TEST_SUITE("ExecutorWakeups") {
    TEST_CASE("idle workers between bursts do not poll on the short timer") {
        // A gap lets idle workers back off from the shortest park. When every
        // worker walks the backoff ladder a gap costs about 8 switches per
        // worker (about 255 here); with one poller it costs about 80, most of
        // them the burst's own wake-ups.
        constexpr std::size_t THREADS = 32;
        constexpr int BURSTS = 20;
        constexpr std::int64_t MAX_SWITCHES_PER_GAP = 120;

        Runtime rt(THREADS);
        auto tiny = []() -> coro::CoroTask<void> { co_return; };
        std::vector<TaskHandle> handles;
        handles.reserve(THREADS);

        const auto before = context_switches();
        for (int burst = 0; burst < BURSTS; ++burst) {
            handles.clear();
            for (std::size_t i = 0; i < THREADS; ++i) {
                handles.push_back(rt.submit(tiny(), "Tiny"));
            }
            for (auto& handle : handles) handle.wait();
            std::this_thread::sleep_for(60ms);
        }
        const auto switches = context_switches() - before;
        rt.shutdown();

        MESSAGE("context switches per gap: " << switches / BURSTS);
        CHECK(switches / BURSTS < MAX_SWITCHES_PER_GAP);
    }
    TEST_CASE("a worker blocked in get() parks while its task waits") {
        // Each round's task waits on a semaphore that this thread releases
        // after WAIT. A waiter that spins burns about the whole wait of CPU,
        // and one that re-checks every millisecond switches about once per
        // millisecond. One that backs off and is woken by the task's end does
        // neither, and returns well before its next timeout; the median of
        // the rounds keeps one slow wake-up from deciding the check.
        constexpr std::size_t THREADS = 4;
        constexpr int ROUNDS = 5;
        constexpr auto WAIT = 40ms;
        constexpr auto MAX_CPU = 40ms;
        constexpr std::int64_t MAX_SWITCHES = 150;
        constexpr auto MAX_MEDIAN_LATENCY = 1ms;

        Runtime rt(THREADS);
        coro::CoroSemaphore gate(1);
        REQUIRE(gate.try_acquire(1));
        std::atomic<std::int64_t> finished_ns{0};

        auto inner = [&]() -> coro::CoroTask<void> {
            co_await gate.acquire(1);
            gate.release(1);
            REQUIRE(gate.try_acquire(1));
        };
        auto outer = [&]() -> coro::CoroTask<void> {
            inner().get();
            finished_ns.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_release);
            co_return;
        };

        // Let the pool settle on its idle backoff first.
        std::this_thread::sleep_for(100ms);
        const auto before = context_switches();
        const auto cpu_before = cpu_time();
        std::vector<std::chrono::nanoseconds> latencies;
        for (int round = 0; round < ROUNDS; ++round) {
            TaskHandle handle = rt.submit(outer(), "Outer");
            std::this_thread::sleep_for(WAIT);
            const auto released = std::chrono::steady_clock::now();
            gate.release(1);
            handle.wait();
            latencies.push_back(std::chrono::nanoseconds(finished_ns.load()) -
                                released.time_since_epoch());
        }
        const auto switches = context_switches() - before;
        const auto cpu = cpu_time() - cpu_before;
        rt.shutdown();

        std::sort(latencies.begin(), latencies.end());
        const auto median = latencies[latencies.size() / 2];
        MESSAGE("context switches over the waits: " << switches);
        MESSAGE("cpu over the waits: " << cpu.count() << " us");
        MESSAGE("median get() return after the release: "
                << std::chrono::duration_cast<std::chrono::microseconds>(median)
                       .count()
                << " us");
        CHECK(cpu < MAX_CPU);
        CHECK(switches < MAX_SWITCHES);
        CHECK(median < MAX_MEDIAN_LATENCY);
    }
}
