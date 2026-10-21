#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <sys/resource.h>

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
}
