// SPDX-License-Identifier: MIT
// Phase-1 mechanism test for the elastic runtime: add_worker/retire_worker on
// ThreadPoolExecutor must be safe under a live stream of work - no stranded
// task, no crash, no UB. Run under the asan/tsan presets for the real value:
// worker churn while the shared run queue is being drained.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_event.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/config.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

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

constexpr std::size_t HALF_RUN = memory::detail::HEAP_PAGE_SIZE + 5000;
constexpr int HALF_BLOCKS = 64;

// Holds its worker until *go, then leaves that worker's heap with idle pages:
// every other block is freed, so its spans stay in the heap.
coro::CoroTask<void> hold_half(std::atomic<int>* started, std::atomic<bool>* go,
                               std::atomic<int>* done,
                               dstd::Vector<void*>* kept) {
    started->fetch_add(1, std::memory_order_acq_rel);
    while (!go->load(std::memory_order_acquire)) std::this_thread::yield();
    void* blocks[HALF_BLOCKS];
    for (void*& b : blocks) {
        b = memory::detail::page_allocate(HALF_RUN, 16);
        std::memset(b, 1, HALF_RUN);
    }
    for (int i = 0; i < HALF_BLOCKS; ++i) {
        if (i % 2 == 0)
            memory::detail::page_deallocate(blocks[i], HALF_RUN);
        else
            kept->push_back(blocks[i]);
    }
    done->fetch_add(1, std::memory_order_release);
    co_return;
}

// Submits a gated burst and returns the largest pool seen before the release.
std::size_t peak_under_backlog(Runtime& rt, ThreadPoolExecutor* exec,
                               std::atomic<int>* done, int n) {
    std::atomic<bool> go{false};
    for (int i = 0; i < n; ++i) rt.submit(gated(&go, done));
    auto deadline = dstd::steady_now() + std::chrono::seconds(10);
    std::size_t peak = exec->live_workers();
    while (peak <= 1 && dstd::steady_now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        peak = std::max(peak, exec->live_workers());
    }
    go.store(true, std::memory_order_release);
    rt.wait_all();
    return peak;
}

// Waits on the pool, as a blocking get() inside a task does, for an event
// that only the blocked worker sets.
coro::CoroTask<void> event_wait(coro::AsyncEvent* event) {
    co_await event->wait();
}

coro::CoroTask<void> get_event(coro::AsyncEvent* event) {
    dftu_utils_test::ok(event_wait(event).get());
    co_return;
}

coro::CoroTask<void> block_then_set(Runtime* rt, coro::AsyncEvent* event,
                                    TaskHandle* waiter) {
    *waiter = rt->submit(get_event(event));
    {
        // Long past the handoff, so a second worker runs the waiter and parks.
        coro::BlockingRegion region;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    event->set();
    co_return;
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
        dstd::Vector<dstd::Thread> churn;
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

    TEST_CASE("nested blocking regions keep every permit") {
        Runtime rt(ExecutorConfig{.thread_count = 2}, false);
        auto nested = []() -> coro::CoroTask<void> {
            {
                coro::BlockingRegion outer;
                coro::BlockingRegion inner;
            }
            co_return;
        };
        for (int round = 0; round < 4; ++round)
            dftu_utils_test::ok(rt.block_on(nested()));

        // A lost permit leaves room for one task at a time.
        std::atomic<int> arrived{0};
        auto meet = [](std::atomic<int>& count) -> coro::CoroTask<bool> {
            count.fetch_add(1, std::memory_order_acq_rel);
            const auto limit = dstd::steady_now() + std::chrono::seconds(10);
            while (count.load(std::memory_order_acquire) < 2 &&
                   dstd::steady_now() < limit) {
                std::this_thread::yield();
            }
            co_return count.load(std::memory_order_acquire) >= 2;
        };
        auto first = rt.submit(meet(arrived));
        auto second = rt.submit(meet(arrived));
        CHECK(dftu_utils_test::ok(first.get()));
        CHECK(dftu_utils_test::ok(second.get()));
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
        Runtime rt(ExecutorConfig{.thread_count = 4, .min_workers = 1});
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        REQUIRE(exec->live_workers() == 1);  // starts at the floor

        std::atomic<int> done{0};
        const int N = 64;
        CHECK(peak_under_backlog(rt, exec, &done, N) > 1);
        CHECK(done.load() == N);

        // Idle now: the monitor idle-retires back down to the floor.
        auto deadline = dstd::steady_now() + std::chrono::seconds(3);
        while (exec->live_workers() > 1 && dstd::steady_now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(exec->live_workers() == 1);  // shrank back to the floor
    }

    TEST_CASE("warm elastic keeps the pool warm past a short idle") {
        // A long keep-alive holds grown workers parked-and-ready across idle
        // gaps instead of retiring them, for near-eager latency.
        Runtime rt(
            ExecutorConfig{.thread_count = 4,
                           .min_workers = 1,
                           .elastic_keepalive = std::chrono::seconds(30)});
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());

        std::atomic<int> done{0};
        REQUIRE(peak_under_backlog(rt, exec, &done, 64) > 1);

        // Idle well under the keep-alive: workers must not have retired yet.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(exec->live_workers() > 1);  // still warm
    }

    // A pool that retires workers wakes every parked one, so a park that never
    // times out must still let an idle worker give its pages back.
    TEST_CASE("idle workers give back their pages while retires wake them") {
        if (memory::config().backend != memory::Backend::PageHeap) return;
        constexpr int WORKERS = 8;
        Runtime rt(ExecutorConfig{.thread_count = WORKERS});
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        REQUIRE(exec->live_workers() == WORKERS);
        std::atomic<int> started{0};
        std::atomic<bool> go{false};
        std::atomic<int> done{0};
        dstd::Vector<void*> kept[WORKERS];
        for (auto& k : kept) k.reserve(HALF_BLOCKS);
        const std::size_t idle_before = memory::detail::page_idle_backed();
        for (int i = 0; i < WORKERS; ++i)
            rt.submit(hold_half(&started, &go, &done, &kept[i]));
        // Each task holds its worker until all have started, so every worker,
        // the ones kept as well as the retired ones, ends with idle pages.
        const auto limit = dstd::steady_now() + std::chrono::seconds(10);
        while (started.load(std::memory_order_acquire) < WORKERS &&
               dstd::steady_now() < limit)
            std::this_thread::yield();
        REQUIRE(started.load() == WORKERS);
        go.store(true, std::memory_order_release);
        rt.wait_all();
        REQUIRE(done.load() == WORKERS);
        const std::size_t idle_after_work = memory::detail::page_idle_backed();
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            exec->retire_worker();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::size_t idle = memory::detail::page_idle_backed();
        for (auto& k : kept)
            for (void* b : k) memory::detail::page_deallocate(b, HALF_RUN);
        CHECK(idle_after_work >= WORKERS * memory::detail::HEAP_PAGE_SIZE);
        CHECK(idle < idle_before + 32 * memory::detail::HEAP_PAGE_SIZE);
    }

    TEST_CASE("a parked driver gives its permit to a worker leaving a block") {
        Runtime rt(1);
        coro::AsyncEvent event;
        TaskHandle waiter;
        auto setter = rt.submit(block_then_set(&rt, &event, &waiter));
        const auto limit = dstd::steady_now() + std::chrono::seconds(10);
        while (!setter.done() && dstd::steady_now() < limit)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(setter.done());
        dftu_utils_test::ok(waiter.wait());
        CHECK(waiter.done());
    }
}
