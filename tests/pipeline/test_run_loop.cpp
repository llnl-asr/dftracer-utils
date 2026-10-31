#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/async_mutex.h>
#include <dftracer/utils/runtime/coro/channel.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/executor/run_loop.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

using namespace dftracer::utils;

namespace {

// Every case here runs on a plain thread with no Runtime, which is the
// whole point: CoroTask::get() has to supply an executor of its own.

coro::CoroTask<int> spawn_and_sum(int n) {
    std::vector<int> results(n, 0);
    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (int i = 0; i < n; ++i) {
            scope.spawn([&results, i](CoroScope&) -> coro::CoroTask<void> {
                results[i] = i + 1;
                co_return;
            });
        }
        co_return;
    });
    co_return std::accumulate(results.begin(), results.end(), 0);
}

// Producers fill a channel narrower than the message count, so they must
// suspend on send and be resumed after the consumer drains. That wakeup is
// what silently vanished when no executor was bound to the thread.
coro::CoroTask<int> produce_consume(int producers, int per_producer) {
    auto channel = coro::make_channel<int>(2);
    int total = 0;

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (int p = 0; p < producers; ++p) {
            scope.spawn([ch = channel->producer(), per_producer](
                            CoroScope&) mutable -> coro::CoroTask<void> {
                auto guard = ch.guard();
                for (int i = 0; i < per_producer; ++i) {
                    if (!co_await ch.send(1)) co_return;
                }
            });
        }

        scope.spawn([&channel, &total](CoroScope&) -> coro::CoroTask<void> {
            while (auto item = co_await channel->receive()) {
                total += *item;
            }
        });
        co_return;
    });

    co_return total;
}

}  // namespace

TEST_SUITE("RunLoop") {
    TEST_CASE("Spawned work completes without a runtime") {
        CHECK(spawn_and_sum(8).get() == 36);
    }

    TEST_CASE("Channel senders resume after the consumer drains") {
        CHECK(produce_consume(4, 25).get() == 100);
    }

    TEST_CASE("Nested get() drives its own subtree") {
        auto outer = []() -> coro::CoroTask<int> {
            int inner = spawn_and_sum(4).get();
            int mine = co_await spawn_and_sum(3);
            co_return inner + mine;
        };
        CHECK(outer().get() == 16);
    }

    TEST_CASE("get() on a worker keeps serving the pool") {
        Runtime rt(2);
        auto task = [&]() -> coro::CoroTask<int> {
            // Runs on a worker, so this nested get() lends that worker to
            // the pool rather than parking it.
            co_return spawn_and_sum(6).get() + produce_consume(2, 10).get();
        };
        CHECK(rt.submit(task()).get() == 41);
        rt.shutdown();
    }

    TEST_CASE("A mutex waiter resumes on the thread of its own loop") {
        coro::AsyncMutex mutex;
        std::atomic<bool> holder_has_lock{false};
        std::atomic<bool> release{false};
        std::atomic<bool> waiter_started{false};
        std::thread::id waiter_thread;
        std::thread::id resumed_thread;

        std::thread holder([&] {
            auto task = [&]() -> coro::CoroTask<void> {
                co_await mutex.lock();
                holder_has_lock.store(true);
                while (!release.load()) std::this_thread::yield();
                mutex.unlock();
            };
            task().get();
        });
        while (!holder_has_lock.load()) std::this_thread::yield();

        std::thread waiter([&] {
            waiter_thread = std::this_thread::get_id();
            auto task = [&]() -> coro::CoroTask<void> {
                waiter_started.store(true);
                co_await mutex.lock();
                resumed_thread = std::this_thread::get_id();
                mutex.unlock();
            };
            task().get();
        });
        while (!waiter_started.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release.store(true);

        holder.join();
        waiter.join();
        CHECK(resumed_thread == waiter_thread);
    }

    TEST_CASE("A stalled loop reports instead of hanging") {
        RunLoop loop;
        bool done = false;
        CHECK_THROWS(loop.drive_until([&] { return done; }));
    }
}
