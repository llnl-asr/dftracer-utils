// Phase-1 mechanism test for the elastic runtime: add_worker/retire_worker on
// ThreadPoolExecutor must be safe under a live stream of work - no stranded
// task, no crash, no UB. Run under the asan/tsan presets for the real value:
// worker churn while the shared run queue is being drained.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/thread_pool_executor.h>
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace dftracer::utils;

namespace {

// Non-capturing coroutine: the counter pointer is a by-value parameter copied
// into the frame, so nothing dangles when the temporary lambda is gone (a
// capturing coroutine-lambda temporary would - its closure outlives the call).
coro::CoroTask<void> bump(std::atomic<int>* done) {
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Holds its worker until *go, so a burst is a real backlog. A short task let
// one worker keep up with a submitter slowed by a loaded CI runner, and the
// pool then rightly never grew.
coro::CoroTask<void> gated(std::atomic<bool>* go, std::atomic<int>* done) {
    while (!go->load(std::memory_order_acquire)) std::this_thread::yield();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Submits a gated burst and returns the largest pool seen before the release.
std::size_t peak_under_backlog(Runtime& rt, ThreadPoolExecutor* exec,
                               std::atomic<int>* done, int n) {
    std::atomic<bool> go{false};
    for (int i = 0; i < n; ++i) rt.submit(gated(&go, done));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::size_t peak = exec->live_workers();
    while (peak <= 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        peak = std::max(peak, exec->live_workers());
    }
    go.store(true, std::memory_order_release);
    rt.wait_all();
    return peak;
}

}  // namespace

TEST_SUITE("DynamicWorkers") {
    TEST_CASE("add/retire under a stream of work strands nothing") {
        Runtime rt(4);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        REQUIRE(exec->live_workers() == 4);

        std::atomic<int> done{0};
        std::atomic<bool> stop{false};

        // Two churners oscillate the pool 2..4 while work drains.
        std::vector<std::thread> churn;
        for (int t = 0; t < 2; ++t)
            churn.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    exec->retire_worker();
                    exec->add_worker();
                }
            });

        const int N = 50000;
        for (int i = 0; i < N; ++i) rt.submit(bump(&done));
        rt.wait_all();

        stop.store(true, std::memory_order_relaxed);
        for (auto& c : churn) c.join();

        CHECK(done.load() == N);
        CHECK(exec->live_workers() >= 1);
        CHECK(exec->live_workers() <= 4);
    }

    TEST_CASE("a shrunk pool still drains all work") {
        Runtime rt(4);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());

        // Retire down to a single worker.
        exec->retire_worker();
        exec->retire_worker();
        exec->retire_worker();
        CHECK(exec->live_workers() == 1);

        std::atomic<int> done{0};
        const int N = 20000;
        for (int i = 0; i < N; ++i) rt.submit(bump(&done));
        rt.wait_all();
        CHECK(done.load() == N);  // one worker drained everything

        // Grow back up; the cap holds.
        exec->add_worker();
        exec->add_worker();
        exec->add_worker();
        CHECK(exec->live_workers() == 4);
        exec->add_worker();  // at cap -> no-op
        CHECK(exec->live_workers() == 4);
    }

    TEST_CASE("retire while idle (parked worker observes retire)") {
        Runtime rt(4);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        // No work submitted: all workers park in wait(). Retiring must still
        // wake and join them.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        exec->retire_worker();
        exec->retire_worker();
        CHECK(exec->live_workers() == 2);
    }

    TEST_CASE("elastic pool grows under load and shrinks when idle") {
        // Floor 1, cap 4: starts small (HPC politeness), grows on demand.
        Runtime rt(ExecutorConfig{.num_threads = 4, .min_workers = 1});
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        REQUIRE(exec->live_workers() == 1);  // starts at the floor

        std::atomic<int> done{0};
        const int N = 64;
        CHECK(peak_under_backlog(rt, exec, &done, N) > 1);
        CHECK(done.load() == N);

        // Idle now: the monitor idle-retires back down to the floor.
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (exec->live_workers() > 1 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(exec->live_workers() == 1);  // shrank back to the floor
    }

    TEST_CASE("warm elastic keeps the pool warm past a short idle") {
        // A long keep-alive holds grown workers parked-and-ready across idle
        // gaps instead of retiring them, for near-eager latency.
        Runtime rt(
            ExecutorConfig{.num_threads = 4,
                           .min_workers = 1,
                           .elastic_keepalive = std::chrono::seconds(30)});
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());

        std::atomic<int> done{0};
        REQUIRE(peak_under_backlog(rt, exec, &done, 64) > 1);

        // Idle well under the keep-alive: workers must not have retired yet.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(exec->live_workers() > 1);  // still warm
    }
}
