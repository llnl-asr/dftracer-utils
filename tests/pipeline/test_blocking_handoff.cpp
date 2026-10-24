// Permit-based blocking handoff: re-entrant run_blocking() must release the
// caller's run slot, keep the pool making progress, and conserve permits. Run
// under asan/tsan for the real value: worker churn and permit park/unpark while
// nested scopes drain. No deadlock, no stranded work, exact permit accounting.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace dftracer::utils;

namespace {

// True once all permits are back and no worker is mid-handoff, observed within
// the deadline. Assert the return, not a re-read: an idle worker briefly takes
// a permit to re-check for work, so a bare available_permits() read races that
// cycle; a real leak never lets it observe cap, so it still returns false.
bool settle(ThreadPoolExecutor* exec, std::size_t cap) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (exec->available_permits() == static_cast<std::ptrdiff_t>(cap) &&
            exec->blocked_workers() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

TEST_SUITE("BlockingHandoff") {
    TEST_CASE("re-entrant run_blocking conserves permits and strands nothing") {
        const std::size_t cap = 4;
        Runtime rt(cap);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());
        // Workers grab a permit on startup then release it when they find no
        // work; the cap invariant holds once that settles.
        REQUIRE(settle(exec, cap));

        std::atomic<int> inner_done{0};
        const int N = 300;

        // Each outer task, on a worker thread, synchronously waits on an inner
        // scope via run_blocking. That parks the worker, so without a handoff a
        // burst of these would starve the pool of runners.
        for (int i = 0; i < N; ++i) {
            rt.scope("outer", [&](CoroScope&) -> coro::CoroTask<void> {
                rt.run_blocking(
                    "inner", [&](CoroScope& is) -> coro::CoroTask<void> {
                        is.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                            inner_done.fetch_add(1, std::memory_order_relaxed);
                            co_return;
                        });
                        co_await is.join();
                    });
                co_return;
            });
        }
        rt.wait_all();

        CHECK(inner_done.load() == N);  // every nested scope ran, no deadlock

        // Permit conservation: every enter_blocking release is matched by an
        // exit_blocking acquire, and idle workers release, so all permits come
        // back and no worker is left mid-handoff.
        CHECK(settle(exec, cap));
        // Live may have grown past the cap for the handoff, but never
        // unbounded.
        CHECK(exec->live_workers() >= cap);
        CHECK(exec->live_workers() <= cap + 256);
    }

    TEST_CASE("deeply nested run_blocking does not deadlock a small pool") {
        // Floor risk: each nesting level parks a worker; the handoff must spawn
        // replacements so the innermost work still finds a runner.
        Runtime rt(2);
        auto* exec = static_cast<ThreadPoolExecutor*>(rt.executor());

        std::atomic<int> reached{0};
        const int DEPTH = 8;

        rt.scope("root", [&](CoroScope&) -> coro::CoroTask<void> {
              std::function<void(int)> nest = [&](int d) {
                  if (d == 0) {
                      reached.fetch_add(1, std::memory_order_relaxed);
                      return;
                  }
                  rt.run_blocking(
                      "level", [&, d](CoroScope& s) -> coro::CoroTask<void> {
                          s.spawn([&, d](CoroScope&) -> coro::CoroTask<void> {
                              nest(d - 1);
                              co_return;
                          });
                          co_await s.join();
                      });
              };
              nest(DEPTH);
              co_return;
          }).get();

        CHECK(reached.load() == 1);
        CHECK(settle(exec, 2));
    }
}
