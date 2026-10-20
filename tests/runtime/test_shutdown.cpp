#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/async_generator.h>
#include <dftracer/utils/runtime/coro/async_mutex.h>
#include <dftracer/utils/runtime/coro/async_once.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/channel.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/join_handle.h>
#include <dftracer/utils/runtime/coro/reserve_async.h>
#include <dftracer/utils/runtime/coro/spawn_future.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/coro/when_any.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace std::chrono_literals;
namespace memory = dftracer::utils::memory;

namespace {

constexpr auto HANG_LIMIT = 10s;
constexpr auto SETTLE = 50ms;
constexpr std::size_t MIB = std::size_t{1} << 20;

struct Probe {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> cancelled{false};
};

// A hang fails the test and ends the process: the stuck threads cannot be
// joined, and ctest must not wait for them.
void fail_hung(const char* what) {
    FAIL_CHECK(what << " hung");
    std::_Exit(1);
}

void shutdown_or_fail(Runtime& rt) {
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    std::thread t([&] {
        rt.shutdown();
        done.set_value();
    });
    if (finished.wait_for(HANG_LIMIT) != std::future_status::ready) {
        fail_hung("executor shutdown");
    }
    t.join();
}

void wait_until_entered(const Probe& probe) {
    const auto deadline = std::chrono::steady_clock::now() + HANG_LIMIT;
    while (!probe.entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            fail_hung("frame start");
        }
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(SETTLE);
}

void wait_or_fail(const TaskHandle& task, const char* what) {
    if (task.future.wait_for(HANG_LIMIT) != std::future_status::ready) {
        fail_hung(what);
    }
}

void expect_cancelled_at_shutdown(Runtime& rt, const Probe& probe) {
    wait_until_entered(probe);
    CHECK_FALSE(probe.finished.load());
    shutdown_or_fail(rt);
    CHECK(probe.finished.load());
    CHECK(probe.cancelled.load());
}

struct Gate {
    std::mutex mutex;
    std::vector<std::coroutine_handle<>> parked;
};

// Parks its awaiter for ever, on no registry: only the test resumes it.
struct NeverReady {
    Gate* gate;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->parked.push_back(h);
    }
    void await_resume() const noexcept {}
};

coro::CoroTask<void> stuck_child(Gate* gate) {
    co_await NeverReady{gate};
    co_return;
}

}  // namespace

TEST_CASE("a frame blocked on each awaitable completes at shutdown") {
    Runtime rt(ExecutorConfig{.num_threads = 2}, false);
    Probe probe;

    SUBCASE("semaphore") {
        coro::CoroSemaphore semaphore(1);
        REQUIRE(semaphore.try_acquire(1));
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                co_await semaphore.acquire(1);
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnSemaphore");
        expect_cancelled_at_shutdown(rt, probe);
        // The cancelled waiter took no permit and left no entry behind.
        semaphore.release(1);
        CHECK(semaphore.try_acquire(1));
    }

    SUBCASE("async mutex") {
        coro::AsyncMutex mutex;
        auto holder = [&]() -> coro::CoroTask<void> {
            co_await mutex.lock();
            co_return;
        };
        wait_or_fail(rt.submit(holder(), "HoldsMutex"), "mutex holder");
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                co_await mutex.lock();
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnMutex");
        expect_cancelled_at_shutdown(rt, probe);
        mutex.unlock();
    }

    SUBCASE("completion latch") {
        Gate gate;
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            std::vector<coro::CoroTask<void>> children;
            children.push_back(stuck_child(&gate));
            children.push_back(stuck_child(&gate));
            try {
                co_await coro::when_all(std::move(children));
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnLatch");
        expect_cancelled_at_shutdown(rt, probe);
        // The children never saw the shutdown; finish them so nothing leaks.
        std::vector<std::coroutine_handle<>> parked;
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            parked.swap(gate.parked);
        }
        for (auto h : parked) h.resume();
    }

    SUBCASE("timer") {
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                co_await coro::timeout(1h, &rt.executor()->get_timer_service());
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnTimer");
        expect_cancelled_at_shutdown(rt, probe);
    }

    SUBCASE("memory reservation") {
        memory::Config config;
        config.budget = 64 * MIB;
        memory::configure(config);
        {
            memory::PoolHandle holds =
                memory::Manager::get().root().child("holds");
            memory::PoolHandle pool =
                memory::Manager::get().root().child("task");
            memory::Reservation held = holds->reserve(64 * MIB);
            REQUIRE(held);
            auto frame = [&]() -> coro::CoroTask<void> {
                probe.entered.store(true);
                auto r = co_await coro::reserve_async(
                    *pool, 32 * MIB,
                    coro::ReserveAsync::Clock::time_point::max());
                probe.cancelled.store(!r &&
                                      r.error() == memory::Status::Cancelled);
                probe.finished.store(true);
                co_return;
            };
            rt.submit(frame(), "BlockedOnReservation");
            expect_cancelled_at_shutdown(rt, probe);
        }
        memory::configure(memory::Config{});
    }

    SUBCASE("channel receive") {
        auto channel = std::make_shared<coro::Channel<int>>(1);
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                co_await channel->receive();
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnReceive");
        expect_cancelled_at_shutdown(rt, probe);
    }

    SUBCASE("channel send") {
        auto channel = std::make_shared<coro::Channel<int>>(1);
        REQUIRE(channel->try_send(1));
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                co_await channel->send(2);
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnSend");
        expect_cancelled_at_shutdown(rt, probe);
    }

    SUBCASE("async once") {
        auto channel = std::make_shared<coro::Channel<int>>(1);
        coro::AsyncOnce<int> once;
        Probe leader;
        auto lead = [&]() -> coro::CoroTask<void> {
            leader.entered.store(true);
            try {
                co_await once.get([&]() -> coro::CoroTask<int> {
                    auto v = co_await channel->receive();
                    co_return v ? *v : 0;
                });
            } catch (const DFTUtilsException& e) {
                leader.cancelled.store(coro::is_cancelled(e));
            }
            leader.finished.store(true);
            co_return;
        };
        rt.submit(lead(), "OnceLeader");
        wait_until_entered(leader);
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                co_await once.get([]() -> coro::CoroTask<int> { co_return 1; });
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "OnceWaiter");
        expect_cancelled_at_shutdown(rt, probe);
        CHECK(leader.finished.load());
    }

    SUBCASE("spawn future") {
        auto state = std::make_shared<coro::SharedState<int>>();
        state->executor = rt.executor();
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            try {
                coro::SpawnFuture<int> future(state);
                co_await future;
            } catch (const DFTUtilsException& e) {
                probe.cancelled.store(coro::is_cancelled(e));
            }
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnSpawnFuture");
        expect_cancelled_at_shutdown(rt, probe);
    }

    SUBCASE("join handle") {
        // A join ends when the tracked frames end, so shutdown wakes the one
        // they wait on and the join follows.
        coro::CoroSemaphore semaphore(1);
        REQUIRE(semaphore.try_acquire(1));
        std::atomic<bool> child_cancelled{false};
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            coro::JoinHandle join;
            auto child = [](coro::CoroSemaphore* sem,
                            std::atomic<bool>* cancelled) -> coro::Coro {
                try {
                    co_await sem->acquire(1);
                } catch (const DFTUtilsException& e) {
                    cancelled->store(coro::is_cancelled(e));
                }
                co_return;
            }(&semaphore, &child_cancelled);
            child.handle().promise().executor = rt.executor();
            join.track(child);
            child.release().resume();
            co_await join.join();
            probe.cancelled.store(child_cancelled.load());
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnJoin");
        expect_cancelled_at_shutdown(rt, probe);
    }

    SUBCASE("read on the IO backend") {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        auto frame = [&]() -> coro::CoroTask<void> {
            probe.entered.store(true);
            char byte = 0;
            ssize_t n = co_await rt.executor()->io_backend().submit_read(
                fds[0], &byte, 1);
            probe.cancelled.store(n == -ECANCELED);
            probe.finished.store(true);
            co_return;
        };
        rt.submit(frame(), "BlockedOnRead");
        expect_cancelled_at_shutdown(rt, probe);
        ::close(fds[0]);
        ::close(fds[1]);
    }
}

// The name stays on one line so the task check can grep for it.
// clang-format off
TEST_CASE("a generator dropped between two yields after a scope fan-out does not terminate") {
    // clang-format on
    Runtime rt(ExecutorConfig{.num_threads = 4}, false);

    struct Source {
        static coro::AsyncGenerator<int> numbers(CoroScope* ctx,
                                                 std::atomic<int>* fanned) {
            co_await ctx->coro_scope(
                [fanned](CoroScope& scope) -> coro::CoroTask<void> {
                    auto a = scope.spawn(
                        [fanned](CoroScope&) -> coro::CoroTask<int> {
                            fanned->fetch_add(1);
                            co_return 1;
                        });
                    auto b = scope.spawn(
                        [fanned](CoroScope&) -> coro::CoroTask<int> {
                            fanned->fetch_add(1);
                            co_return 2;
                        });
                    co_await a;
                    co_await b;
                });
            co_yield 1;
            co_yield 2;
        }
    };

    std::atomic<int> fanned{0};
    std::atomic<int> first{0};
    TaskHandle task =
        rt.scope("DropGenerator", [&](CoroScope& ctx) -> coro::CoroTask<void> {
            {
                auto gen = Source::numbers(&ctx, &fanned);
                auto value = co_await gen.next();
                first.store(value ? *value : -1);
            }
            co_return;
        });
    wait_or_fail(task, "generator task");

    CHECK(first.load() == 1);
    CHECK(fanned.load() == 2);
    shutdown_or_fail(rt);
}

TEST_CASE("when_any cancels a CoroTask loser that never ends") {
    Runtime rt(ExecutorConfig{.num_threads = 4}, false);

    std::atomic<std::size_t> winner_index{99};
    std::atomic<bool> loser_started{false};
    std::atomic<bool> loser_gone{false};
    auto frame = [&]() -> coro::CoroTask<void> {
        auto winner = []() -> coro::CoroTask<int> {
            co_await coro::yield();
            co_return 1;
        };
        // A cancelled task ends at its wait without resuming, so its frame is
        // destroyed with the destructors of its locals and nothing is thrown.
        auto loser = [](std::atomic<bool>* started,
                        std::atomic<bool>* gone) -> coro::CoroTask<int> {
            struct Guard {
                std::atomic<bool>* gone;
                ~Guard() { gone->store(true); }
            } guard{gone};
            started->store(true);
            for (;;) co_await coro::yield();
        };
        auto result = co_await coro::when_any(
            winner(), loser(&loser_started, &loser_gone));
        winner_index.store(result.index);
        co_return;
    };
    wait_or_fail(rt.submit(frame(), "CancelsNeverEndingLoser"),
                 "when_any with a never-ending CoroTask loser");

    CHECK(winner_index.load() == 0);
    // A loser that began is gone; one that the winner beat to the start never
    // ran.
    CHECK(loser_started.load() == loser_gone.load());
    shutdown_or_fail(rt);
}
