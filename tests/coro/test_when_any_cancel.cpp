#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/channel.h>
#include <dftracer/utils/runtime/coro/when_any.h>
#include <dftracer/utils/runtime/pipeline/scheduler.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dftracer::utils;

namespace {

constexpr int NOT_STOPPED = -1;
constexpr int NO_VALUE = -1;
constexpr int WINNER_VALUE = 7;

using Clock = std::chrono::steady_clock;
constexpr auto WINNER_TIME = std::chrono::milliseconds(50);
constexpr auto LOSER_TIME = std::chrono::seconds(2);
constexpr auto SLACK = std::chrono::milliseconds(100);

struct Race {
    std::atomic<int> clock{0};
    std::array<std::atomic<int>, 3> stopped_at;
    std::array<std::atomic<bool>, 3> saw_token;
    std::array<std::shared_ptr<coro::Channel<int>>, 3> channels;

    Race() {
        for (std::size_t i = 0; i < channels.size(); ++i) {
            stopped_at[i].store(NOT_STOPPED);
            saw_token[i].store(false);
            channels[i] = std::make_shared<coro::Channel<int>>(1);
        }
    }
};

using TokenPtr = std::shared_ptr<std::atomic<bool>>;

coro::CoroTask<int> receive_and_record(std::shared_ptr<Race> race,
                                       TokenPtr token, std::size_t slot) {
    auto value = co_await race->channels[slot]->receive();
    race->saw_token[slot].store(token->load());
    race->stopped_at[slot].store(race->clock.fetch_add(1));
    co_return value ? *value : NO_VALUE;
}

// Blocks on its channel. The token is the cooperative signal; cancel() closes
// the channel so the blocked receive wakes and the loser can stop.
template <int Tag>
class Racer {
   public:
    using result_type = int;

    Racer(std::shared_ptr<Race> race, std::size_t slot,
          bool throw_on_cancel = false)
        : race_(std::move(race)),
          slot_(slot),
          throw_on_cancel_(throw_on_cancel),
          token_(std::make_shared<std::atomic<bool>>(false)),
          task_(receive_and_record(race_, token_, slot_)) {}

    bool await_ready() { return task_.await_ready(); }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        return task_.await_suspend(h);
    }

    int await_resume() { return task_.await_resume(); }

    TokenPtr get_cancellation_token() const { return token_; }

    void cancel() {
        race_->channels[slot_]->close();
        if (throw_on_cancel_) throw std::runtime_error("cancel failed");
    }

   private:
    std::shared_ptr<Race> race_;
    std::size_t slot_;
    bool throw_on_cancel_;
    TokenPtr token_;
    coro::CoroTask<int> task_;
};

// Forwards a task. It has no cancel() and no token, so when_any neither stops
// it nor drops it before it starts.
template <typename T>
class Plain {
   public:
    using result_type = T;

    explicit Plain(coro::CoroTask<T> task) : task_(std::move(task)) {}

    bool await_ready() { return task_.await_ready(); }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        return task_.await_suspend(h);
    }

    T await_resume() { return task_.await_resume(); }

   private:
    coro::CoroTask<T> task_;
};

std::thread feed_winner_later(const std::shared_ptr<Race>& race,
                              std::size_t slot) {
    return std::thread([race, slot] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        race->channels[slot]->try_send(WINNER_VALUE);
    });
}

}  // namespace

TEST_CASE("when_any - no loser runs after when_any returns") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    auto race = std::make_shared<Race>();
    std::atomic<int> winner_index{-1};
    std::atomic<int> winner_value{NO_VALUE};
    std::atomic<int> returned_at{NOT_STOPPED};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            auto result = co_await coro::when_any(
                Racer<0>(race, 0), Racer<0>(race, 1), Racer<0>(race, 2));
            returned_at.store(race->clock.fetch_add(1));
            winner_index.store(static_cast<int>(result.index));
            winner_value.store(result.result);
            co_return;
        },
        "NoLoserRunsAfterWhenAny");

    auto feeder = feed_winner_later(race, 1);
    scheduler.schedule(task);
    task->wait();
    feeder.join();

    CHECK(winner_index.load() == 1);
    CHECK(winner_value.load() == WINNER_VALUE);
    for (std::size_t i = 0; i < race->stopped_at.size(); ++i) {
        CAPTURE(i);
        CHECK(race->stopped_at[i].load() != NOT_STOPPED);
        CHECK(race->stopped_at[i].load() < returned_at.load());
        if (i != 1) {
            // A loser blocked on its channel was woken by cancel(), not fed.
            CHECK(race->saw_token[i].load());
            CHECK(race->channels[i]->is_closed());
        }
    }
    executor.shutdown();
}

TEST_CASE("when_any - no loser runs after when_any returns (heterogeneous)") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    auto race = std::make_shared<Race>();
    std::atomic<int> winner_index{-1};
    std::atomic<int> returned_at{NOT_STOPPED};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            auto result = co_await coro::when_any(
                Racer<0>(race, 0), Racer<1>(race, 1), Racer<2>(race, 2));
            returned_at.store(race->clock.fetch_add(1));
            winner_index.store(static_cast<int>(result.index));
            co_return;
        },
        "NoLoserRunsAfterWhenAnyHetero");

    auto feeder = feed_winner_later(race, 2);
    scheduler.schedule(task);
    task->wait();
    feeder.join();

    CHECK(winner_index.load() == 2);
    for (std::size_t i = 0; i < race->stopped_at.size(); ++i) {
        CAPTURE(i);
        CHECK(race->stopped_at[i].load() != NOT_STOPPED);
        CHECK(race->stopped_at[i].load() < returned_at.load());
        if (i != 2) CHECK(race->saw_token[i].load());
    }
    executor.shutdown();
}

TEST_CASE("when_any - a spawned loser stops when cancelled") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> loser_saw_cancel{false};
    std::atomic<bool> loser_finished{false};
    std::atomic<bool> loser_finished_before_return{false};
    std::atomic<long long> elapsed_ms{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto winner = ctx.spawn([](CoroScope&) -> coro::CoroTask<int> {
                std::this_thread::sleep_for(WINNER_TIME);
                co_return 1;
            });
            auto loser =
                ctx.spawn([&](CoroScope& scope) -> coro::CoroTask<int> {
                    const auto deadline = Clock::now() + LOSER_TIME;
                    while (Clock::now() < deadline) {
                        if (scope.is_cancellation_requested()) {
                            loser_saw_cancel.store(true);
                            break;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(5));
                    }
                    loser_finished.store(true);
                    co_return 2;
                });

            std::vector<coro::SpawnFuture<int>> futures;
            futures.push_back(std::move(winner));
            futures.push_back(std::move(loser));
            const auto start = Clock::now();
            auto result = co_await coro::when_any(std::move(futures));
            elapsed_ms.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - start)
                    .count());
            loser_finished_before_return.store(loser_finished.load());
            CHECK(result.index == 0);
            co_return;
        },
        "SpawnedLoserStops");

    scheduler.schedule(task);
    task->wait();

    CHECK(loser_saw_cancel.load());
    CHECK(loser_finished_before_return.load());
    CHECK(elapsed_ms.load() <
          std::chrono::duration_cast<std::chrono::milliseconds>(WINNER_TIME +
                                                                SLACK)
              .count());
    executor.shutdown();
}

TEST_CASE("when_any - a winner that throws after it won delivers its error") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    auto race = std::make_shared<Race>();
    std::atomic<bool> threw{false};
    std::atomic<bool> returned{false};
    std::atomic<int> returned_at{NOT_STOPPED};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            try {
                // Loser 0 throws from cancel() once slot 1 has won. Loser 2
                // must still be cancelled and awaited.
                co_await coro::when_any(Racer<0>(race, 0, true),
                                        Racer<0>(race, 1), Racer<0>(race, 2));
            } catch (const std::runtime_error&) {
                threw.store(true);
            }
            returned_at.store(race->clock.fetch_add(1));
            returned.store(true);
            co_return;
        },
        "WinnerThrowsAfterWinning");

    auto feeder = feed_winner_later(race, 1);
    scheduler.schedule(task);
    task->wait();
    feeder.join();

    CHECK(returned.load());
    CHECK(threw.load());
    for (std::size_t i = 0; i < race->stopped_at.size(); ++i) {
        CAPTURE(i);
        CHECK(race->stopped_at[i].load() != NOT_STOPPED);
        CHECK(race->stopped_at[i].load() < returned_at.load());
    }
    executor.shutdown();
}

TEST_CASE("when_any - a loser with no token and no cancel() runs to its end") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> loser_ended{false};
    std::atomic<bool> ended_before_return{false};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            auto winner = []() -> coro::CoroTask<int> { co_return 1; };
            auto loser = [](std::atomic<bool>* ended) -> coro::CoroTask<int> {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ended->store(true);
                co_return 2;
            };
            auto result = co_await coro::when_any(
                Plain<int>(winner()), Plain<int>(loser(&loser_ended)));
            ended_before_return.store(loser_ended.load());
            CHECK(result.index == 0);
            co_return;
        },
        "UncancellableLoserRunsToEnd");

    scheduler.schedule(task);
    task->wait();

    CHECK(ended_before_return.load());
    executor.shutdown();
}

TEST_CASE("when_any with a timeout loser returns when the winner does") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    constexpr auto TIMEOUT = std::chrono::seconds(5);
    std::atomic<long long> elapsed_ms{-1};
    std::atomic<std::size_t> winner_index{99};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            auto winner = []() -> coro::CoroTask<void> {
                std::this_thread::sleep_for(WINNER_TIME);
                co_return;
            };
            const auto start = Clock::now();
            auto result = co_await coro::when_any(
                winner(),
                coro::timeout(TIMEOUT, &executor.get_timer_service()));
            elapsed_ms.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - start)
                    .count());
            winner_index.store(result.index);
            co_return;
        },
        "TimeoutLoser");

    scheduler.schedule(task);
    task->wait();

    CHECK(winner_index.load() == 0);
    CHECK(elapsed_ms.load() >= 0);
    CHECK(elapsed_ms.load() <
          std::chrono::duration_cast<std::chrono::milliseconds>(WINNER_TIME +
                                                                SLACK)
              .count());
    executor.shutdown();
}

namespace {

// A throw would resume a frame into its catch block, which records it. A
// cancelled frame instead ends at its wait without resuming, so the record
// stays empty, and the destructor of a local shows the frame was destroyed.
struct Seen {
    std::atomic<bool> started{false};
    std::atomic<bool> threw{false};
    std::atomic<int> destroyed{0};
};

struct Guard {
    std::shared_ptr<Seen> seen;
    ~Guard() { seen->destroyed.fetch_add(1); }
};

coro::CoroTask<int> spins_at_a_wait(std::shared_ptr<Seen> seen) {
    Guard guard{seen};
    try {
        seen->started.store(true);
        for (;;) co_await coro::yield();
    } catch (...) {
        seen->threw.store(true);
        throw;
    }
}

// Waits on a task that waits for ever.
coro::CoroTask<int> awaits_a_spinner(std::shared_ptr<Seen> seen) {
    Guard guard{seen};
    try {
        co_return co_await spins_at_a_wait(seen);
    } catch (...) {
        seen->threw.store(true);
        throw;
    }
}

coro::CoroTask<int> finishes_after(std::chrono::milliseconds time) {
    std::this_thread::sleep_for(time);
    co_return 1;
}

}  // namespace

TEST_CASE("when_any - a cancelled CoroTask loser stops without throwing") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    auto seen = std::make_shared<Seen>();
    int frames = 1;
    std::atomic<long long> elapsed_ms{-1};
    std::atomic<std::size_t> winner_index{99};

    auto run = [&](auto make_loser, const char* name) {
        auto task = make_task(
            [&](CoroScope&) -> coro::CoroTask<void> {
                const auto start = Clock::now();
                auto result = co_await coro::when_any(
                    make_loser(seen), finishes_after(WINNER_TIME));
                elapsed_ms.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - start)
                        .count());
                winner_index.store(result.index);
                co_return;
            },
            name);
        scheduler.schedule(task);
        task->wait();
    };

    SUBCASE("the loser waits itself") {
        run(spins_at_a_wait, "CancelledLoserNoThrow");
    }

    SUBCASE("the loser awaits a task that waits") {
        frames = 2;
        run(awaits_a_spinner, "CancelledNestedLoserNoThrow");
    }

    CHECK(winner_index.load() == 1);
    CHECK(seen->started.load());
    CHECK_FALSE(seen->threw.load());
    CHECK(seen->destroyed.load() == frames);
    CHECK(elapsed_ms.load() <
          std::chrono::duration_cast<std::chrono::milliseconds>(WINNER_TIME +
                                                                SLACK)
              .count());
    executor.shutdown();
}

TEST_CASE("when_any - a CoroTask loser that never started is dropped") {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> loser_ran{false};
    std::atomic<std::size_t> winner_index{99};

    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> {
            auto winner = []() -> coro::CoroTask<int> { co_return 1; };
            auto loser = [](std::atomic<bool>* ran) -> coro::CoroTask<int> {
                ran->store(true);
                std::this_thread::sleep_for(LOSER_TIME);
                co_return 2;
            };
            auto result = co_await coro::when_any(winner(), loser(&loser_ran));
            winner_index.store(result.index);
            co_return;
        },
        "UnstartedLoserDropped");

    scheduler.schedule(task);
    task->wait();

    CHECK(winner_index.load() == 0);
    CHECK_FALSE(loser_ran.load());
    executor.shutdown();
}
