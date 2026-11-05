// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_event.h>
#include <dftracer/utils/coro/async_latch.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/retry.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace std::chrono_literals;

namespace {

CoroTask<int> value_after(int value, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    co_return value;
}

CoroTask<int> value_nested(int value, std::chrono::milliseconds delay) {
    const int inner = co_await value_after(value, delay);
    co_return inner + 1;
}

CoroTask<int> fails_after(std::chrono::milliseconds delay) {
    co_await sleep(delay);
    throw common::DFTUtilsException(
        dstd::Error{dstd::Condition::Io, "planned failure"});
}

CoroTask<void> nothing_after(std::chrono::milliseconds delay) {
    co_await sleep(delay);
}

CoroTask<void> failing_void(std::chrono::milliseconds delay) {
    co_await sleep(delay);
    throw common::DFTUtilsException(
        dstd::Error{dstd::Condition::Io, "planned failure"});
}

CoroTask<void> count_after(std::atomic<int>& finished,
                           std::chrono::milliseconds delay) {
    co_await sleep(delay);
    finished.fetch_add(1);
}

CoroTask<void> nested_scopes() {
    co_await with_scope([](Scope& outer) -> CoroTask<void> {
        outer.spawn([]() -> CoroTask<void> {
            co_await with_scope([](Scope& inner) -> CoroTask<void> {
                inner.spawn(nothing_after(30s));
                co_return;
            });
        });
        co_return;
    });
}

CoroTask<void> latch_arrive(AsyncLatch& latch) {
    co_await sleep(1ms);
    latch.count_down();
}

CoroTask<void> latch_wait(AsyncLatch& latch, std::atomic<int>& released) {
    co_await latch.wait();
    released.fetch_add(1);
}

CoroTask<void> event_wait(AsyncEvent& event, std::atomic<int>& released) {
    co_await event.wait();
    released.fetch_add(1);
}

CoroTask<int> tracked_job(int value, std::atomic<int>& running,
                          std::atomic<int>& peak) {
    const int now = running.fetch_add(1) + 1;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
    co_await sleep(2ms);
    running.fetch_sub(1);
    co_return value * 2;
}

struct DoubleJob {
    std::atomic<int>* running;
    std::atomic<int>* peak;
    CoroTask<int> operator()(int value) const {
        return tracked_job(value, *running, *peak);
    }
};

}  // namespace

TEST_CASE("sleep waits at least its duration and does not raise") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    runtime.block_on(nothing_after(20ms));
    CHECK(std::chrono::steady_clock::now() - start >= 20ms);
}

TEST_CASE("with_timeout gives the value of a task that ends in time") {
    Runtime runtime(2);
    CHECK(runtime.block_on(with_timeout(value_after(7, 1ms), 5s)) == 7);
}

TEST_CASE("with_timeout raises a timeout error and cancels a slow task") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_timeout(value_after(7, 30s), 10ms));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("with_timeout wakes a parked wait in a nested task") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_timeout(value_nested(7, 30s), 10ms));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("with_timeout wakes a task parked on a channel, event or semaphore") {
    Runtime runtime(2);
    Channel<int> channel(1);
    AsyncEvent event;
    AsyncSemaphore semaphore(1);
    semaphore.try_acquire(1);
    auto on_channel = [&]() -> CoroTask<void> {
        auto item = co_await channel.receive();
        CHECK(!item);
    };
    auto on_event = [&]() -> CoroTask<void> { co_await event.wait(); };
    auto on_semaphore = [&]() -> CoroTask<void> {
        co_await semaphore.acquire(1);
    };
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(runtime.block_on(with_timeout(on_channel(), 5ms)));
    CHECK_THROWS(runtime.block_on(with_timeout(on_event(), 5ms)));
    CHECK_THROWS(runtime.block_on(with_timeout(on_semaphore(), 5ms)));
    CHECK(std::chrono::steady_clock::now() - start < 10s);
    semaphore.release(1);
}

TEST_CASE("a cancel that lands before the wait parks is not lost") {
    Runtime runtime(4);
    for (int round = 0; round < 200; ++round) {
        AsyncEvent event;
        std::atomic<bool> go{false};
        auto waiter = [&]() -> CoroTask<void> {
            while (!go.load()) co_await yield();
            co_await event.wait();
        };
        dstd::Vector<CoroTask<void>> tasks;
        tasks.push_back(waiter());
        go.store(true);
        CHECK_THROWS(runtime.block_on(with_timeout(std::move(tasks[0]), 1us)));
    }
}

TEST_CASE("with_deadline in the past times out a waiting task") {
    Runtime runtime(2);
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_deadline(value_after(1, 30s),
                                       std::chrono::steady_clock::now() - 1s));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Timeout);
}

TEST_CASE("retry runs until success and stops at max_attempts") {
    Runtime runtime(2);
    int calls = 0;
    auto flaky = [&]() -> CoroTask<int> {
        ++calls;
        if (calls < 3) {
            throw common::DFTUtilsException(
                dstd::Error{dstd::Condition::Io, "flaky"});
        }
        co_return 42;
    };
    RetryPolicy policy;
    policy.max_attempts = 5;
    policy.initial_backoff = 1ms;
    CHECK(runtime.block_on(retry(flaky, std::move(policy))) == 42);
    CHECK(calls == 3);

    int failures = 0;
    auto always = [&]() -> CoroTask<void> {
        ++failures;
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::Io, "always"});
        co_return;
    };
    RetryPolicy limited;
    limited.initial_backoff = 1ms;
    limited.max_attempts = 4;
    CHECK_THROWS_AS(runtime.block_on(retry(always, std::move(limited))),
                    common::DFTUtilsException);
    CHECK(failures == 4);
}

TEST_CASE("retry obeys the predicate and never retries a cancelled error") {
    Runtime runtime(2);
    int calls = 0;
    auto failing = [&]() -> CoroTask<int> {
        ++calls;
        throw common::DFTUtilsException(dstd::Error{dstd::Condition::Io, "no"});
        co_return 0;
    };
    RetryPolicy policy;
    policy.initial_backoff = 1ms;
    policy.should_retry = [](std::exception_ptr) { return false; };
    CHECK_THROWS(runtime.block_on(retry(failing, std::move(policy))));
    CHECK(calls == 1);

    int cancelled_calls = 0;
    auto cancelled = [&]() -> CoroTask<int> {
        ++cancelled_calls;
        throw_cancelled("stop");
        co_return 0;
    };
    RetryPolicy plain;
    plain.initial_backoff = 1ms;
    CHECK_THROWS(runtime.block_on(retry(cancelled, std::move(plain))));
    CHECK(cancelled_calls == 1);
}

TEST_CASE("retry backoff grows by the multiplier up to the cap") {
    RetryPolicy policy;
    policy.initial_backoff = 10ms;
    policy.multiplier = 2.0;
    policy.max_backoff = 25ms;
    CHECK(coro::detail::retry_backoff(policy, 1) == 10ms);
    CHECK(coro::detail::retry_backoff(policy, 2) == 20ms);
    CHECK(coro::detail::retry_backoff(policy, 3) == 25ms);
    policy.jitter = 0.5;
    for (int i = 0; i < 100; ++i) {
        const auto waited = coro::detail::retry_backoff(policy, 1);
        CHECK(waited >= 10ms);
        CHECK(waited <= 15ms);
    }
}

TEST_CASE("when_all_settled gives each outcome in task order") {
    Runtime runtime(2);
    dstd::Vector<CoroTask<int>> tasks;
    tasks.push_back(value_after(1, 5ms));
    tasks.push_back(fails_after(1ms));
    tasks.push_back(value_after(3, 1ms));
    auto outcomes = runtime.block_on(when_all_settled(std::move(tasks)));
    REQUIRE(outcomes.size() == 3);
    REQUIRE(outcomes[0].has_value());
    CHECK(*outcomes[0] == 1);
    REQUIRE_FALSE(outcomes[1].has_value());
    CHECK(outcomes[1].error().condition() == dstd::Condition::Io);
    REQUIRE(outcomes[2].has_value());
    CHECK(*outcomes[2] == 3);
}

TEST_CASE("when_all_settled takes void tasks") {
    Runtime runtime(2);
    dstd::Vector<CoroTask<void>> tasks;
    tasks.push_back(nothing_after(1ms));
    auto outcomes = runtime.block_on(when_all_settled(std::move(tasks)));
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0].has_value());
}

TEST_CASE("first_success skips failures and cancels the slow loser") {
    Runtime runtime(2);
    dstd::Vector<CoroTask<int>> tasks;
    tasks.push_back(fails_after(1ms));
    tasks.push_back(value_after(9, 5ms));
    tasks.push_back(value_after(1, 30s));
    const auto start = std::chrono::steady_clock::now();
    CHECK(runtime.block_on(first_success(std::move(tasks))) == 9);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("first_success raises the last error when all fail") {
    Runtime runtime(2);
    dstd::Vector<CoroTask<int>> tasks;
    tasks.push_back(fails_after(1ms));
    tasks.push_back(fails_after(2ms));
    CHECK_THROWS_AS(runtime.block_on(first_success(std::move(tasks))),
                    common::DFTUtilsException);
    dstd::Vector<CoroTask<int>> none;
    CHECK_THROWS_AS(runtime.block_on(first_success(std::move(none))),
                    common::DFTUtilsException);
}

TEST_CASE("first_success cancelled mid-wait cancels its tasks and joins") {
    Runtime runtime(2);
    dstd::Vector<CoroTask<int>> tasks;
    tasks.push_back(value_after(1, 30s));
    tasks.push_back(value_after(2, 30s));
    const auto start = std::chrono::steady_clock::now();
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_timeout(first_success(std::move(tasks)), 10ms));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("when_all_settled cancelled mid-wait cancels its tasks and joins") {
    Runtime runtime(2);
    dstd::Vector<CoroTask<int>> tasks;
    tasks.push_back(value_after(1, 30s));
    tasks.push_back(value_after(2, 30s));
    const auto start = std::chrono::steady_clock::now();
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(
            with_timeout(when_all_settled(std::move(tasks)), 10ms));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("parallel_map keeps item order and the in-flight limit") {
    Runtime runtime(8);
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    dstd::Vector<int> items;
    for (int i = 0; i < 40; ++i) items.push_back(i);
    ParallelOptions options;
    options.max_in_flight = 3;
    auto results = runtime.block_on(
        parallel_map(std::move(items), DoubleJob{&running, &peak}, options));
    REQUIRE(results.size() == 40);
    for (int i = 0; i < 40; ++i) CHECK(results[i] == i * 2);
    CHECK(peak.load() <= 3);
    CHECK(peak.load() >= 2);
}

TEST_CASE("parallel_map fails fast and does not start later jobs") {
    Runtime runtime(2);
    std::atomic<int> started{0};
    dstd::Vector<int> items;
    for (int i = 0; i < 1000; ++i) items.push_back(i);
    ParallelOptions options;
    options.max_in_flight = 2;
    auto job = [&](int) -> CoroTask<void> {
        started.fetch_add(1);
        co_await sleep(1ms);
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::Io, "stop here"});
    };
    CHECK_THROWS_AS(
        runtime.block_on(parallel_map(std::move(items), job, options)),
        common::DFTUtilsException);
    CHECK(started.load() < 100);
}

TEST_CASE("parallel_map takes an empty list") {
    Runtime runtime(2);
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    auto results = runtime.block_on(
        parallel_map(dstd::Vector<int>{}, DoubleJob{&running, &peak}));
    CHECK(results.empty());
}

TEST_CASE("parallel_map_unordered yields every result once") {
    Runtime runtime(4);
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    dstd::Vector<int> items;
    for (int i = 0; i < 50; ++i) items.push_back(i);
    ParallelOptions options;
    options.max_in_flight = 4;
    dstd::Vector<int> seen;
    runtime.block_on([&]() -> CoroTask<void> {
        auto stream = parallel_map_unordered(
            std::move(items), DoubleJob{&running, &peak}, options);
        for (;;) {
            auto value = co_await stream.next();
            if (!value) break;
            seen.push_back(*value);
        }
    }());
    std::sort(seen.begin(), seen.end());
    REQUIRE(seen.size() == 50);
    for (int i = 0; i < 50; ++i) CHECK(seen[i] == i * 2);
    CHECK(peak.load() <= 4);
}

TEST_CASE("parallel_map_unordered survives a reader that stops early") {
    Runtime runtime(4);
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    dstd::Vector<int> items;
    for (int i = 0; i < 1000; ++i) items.push_back(i);
    ParallelOptions options;
    options.max_in_flight = 2;
    int first = -1;
    runtime.block_on([&]() -> CoroTask<void> {
        auto stream = parallel_map_unordered(
            std::move(items), DoubleJob{&running, &peak}, options);
        auto value = co_await stream.next();
        first = *value;
    }());
    CHECK(first >= 0);
    runtime.block_on(nothing_after(50ms));
    CHECK(running.load() == 0);
}

TEST_CASE("parallel_map_unordered raises a job error from the read") {
    Runtime runtime(2);
    dstd::Vector<int> items;
    items.push_back(1);
    auto job = [](int) -> CoroTask<int> {
        co_await sleep(1ms);
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::Io, "bad job"});
    };
    dstd::Condition condition = dstd::Condition::Unknown;
    runtime.block_on([&]() -> CoroTask<void> {
        auto stream = parallel_map_unordered(std::move(items), job);
        try {
            auto value = co_await stream.next();
            CHECK(!value);
        } catch (const common::DFTUtilsException& e) {
            condition = e.condition();
        }
    }());
    CHECK(condition == dstd::Condition::Io);
}

TEST_CASE("a child error wakes a parked sibling and join raises that error") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_scope([](Scope& scope) -> CoroTask<void> {
            scope.spawn(nothing_after(30s));
            scope.spawn(nothing_after(30s));
            scope.spawn(failing_void(1ms));
            co_return;
        }));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Io);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("a child that was only cancelled does not stop its siblings") {
    Runtime runtime(2);
    std::atomic<int> finished{0};
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_scope([&](Scope& scope) -> CoroTask<void> {
            scope.spawn([]() -> CoroTask<void> {
                co_await sleep(1ms);
                throw_cancelled("from outside");
            });
            scope.spawn(count_after(finished, 30ms));
            co_return;
        }));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Cancelled);
    CHECK(finished.load() == 1);
}

TEST_CASE(
    "a supervisor scope lets siblings finish and raises the first error") {
    Runtime runtime(2);
    std::atomic<int> finished{0};
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        runtime.block_on(with_scope(
            [&](Scope& scope) -> CoroTask<void> {
                scope.spawn(failing_void(1ms));
                scope.spawn(count_after(finished, 30ms));
                co_return;
            },
            ScopeMode::Supervisor));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Io);
    CHECK(finished.load() == 1);
}

TEST_CASE("request_stop ends parked children and join raises nothing") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    runtime.block_on(with_scope([](Scope& scope) -> CoroTask<void> {
        scope.spawn(nothing_after(30s));
        co_await sleep(1ms);
        scope.request_stop();
    }));
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("cancelling a task reaches the scopes nested under it") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(runtime.block_on(with_timeout(nested_scopes(), 10ms)));
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("cancelling parallel_map wakes its running jobs") {
    Runtime runtime(2);
    dstd::Vector<int> items;
    for (int i = 0; i < 8; ++i) items.push_back(i);
    auto job = [](int) -> CoroTask<void> { co_await sleep(30s); };
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(runtime.block_on(
        with_timeout(parallel_map(std::move(items), job), 10ms)));
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("a cancelled parallel_map with no results still reports the cancel") {
    Runtime runtime(2);
    dstd::Vector<int> items;
    for (int i = 0; i < 4000; ++i) items.push_back(i);
    ParallelOptions options;
    options.max_in_flight = 1;
    auto job = [](int) -> CoroTask<void> { co_await sleep(1ms); };
    CHECK_THROWS(runtime.block_on(
        with_timeout(parallel_map(std::move(items), job, options), 5ms)));
}

TEST_CASE("a dropped child source does not stay in its parent") {
    StopSource parent;
    for (int i = 0; i < 1000; ++i) {
        StopSource child(parent.token());
        CHECK_FALSE(child.token().stop_requested());
    }
    StopSource kept(parent.token());
    parent.request_stop();
    CHECK(kept.token().stop_requested());
}

TEST_CASE("a stop token stops a linked task and every source below it") {
    Runtime runtime(2);
    StopSource root;
    StopSource child(root.token());
    StopSource late(root.token());
    runtime.block_on([&]() -> CoroTask<void> {
        Scope scope;
        auto waiting = scope.spawn_future(
            with_stop_token(nothing_after(30s), child.token()));
        co_await sleep(5ms);
        CHECK_FALSE(child.token().stop_requested());
        root.request_stop();
        bool stopped = false;
        try {
            co_await waiting;
        } catch (const common::DFTUtilsException& e) {
            stopped = is_cancelled(e);
        }
        CHECK(stopped);
        co_await scope.join();
    }());
    CHECK(late.token().stop_requested());
    StopSource after_stop(root.token());
    CHECK(after_stop.token().stop_requested());
    CHECK_FALSE(StopToken{}.stop_requested());
}

TEST_CASE("this_coro::stop_requested lets a computing task see the stop") {
    Runtime runtime(2);
    std::atomic<std::uint64_t> spins{0};
    auto spin = [&]() -> CoroTask<void> {
        for (;;) {
            const bool stop = co_await this_coro::stop_requested;
            if (stop) co_return;
            spins.fetch_add(1);
        }
    };
    runtime.block_on(with_scope([&](Scope& scope) -> CoroTask<void> {
        scope.spawn(spin());
        co_await sleep(2ms);
        scope.request_stop();
    }));
    CHECK(spins.load() > 0);
}

TEST_CASE("AsyncEvent wakes every waiter and later waits pass at once") {
    Runtime runtime(4);
    AsyncEvent event;
    std::atomic<int> released{0};
    runtime.block_on([&]() -> CoroTask<void> {
        Scope scope;
        for (int i = 0; i < 64; ++i) scope.spawn(event_wait(event, released));
        co_await sleep(5ms);
        CHECK(released.load() == 0);
        event.set();
        co_await scope.join();
        co_await event.wait();
    }());
    CHECK(released.load() == 64);
    CHECK(event.is_set());
    event.reset();
    CHECK_FALSE(event.is_set());
}

TEST_CASE("AsyncLatch releases waiters at zero") {
    Runtime runtime(4);
    AsyncLatch latch(16);
    std::atomic<int> released{0};
    CHECK_FALSE(latch.try_wait());
    runtime.block_on([&]() -> CoroTask<void> {
        Scope scope;
        for (int i = 0; i < 8; ++i) scope.spawn(latch_wait(latch, released));
        for (int i = 0; i < 16; ++i) scope.spawn(latch_arrive(latch));
        co_await scope.join();
    }());
    CHECK(released.load() == 8);
    CHECK(latch.try_wait());
    AsyncLatch zero(0);
    CHECK(zero.try_wait());
}

TEST_CASE("AsyncSemaphore permit gives its units back on destruction") {
    Runtime runtime(2);
    AsyncSemaphore semaphore(2);
    runtime.block_on([&]() -> CoroTask<void> {
        {
            auto permit = co_await semaphore.permit(2);
            CHECK_FALSE(semaphore.try_acquire(1));
        }
        CHECK(semaphore.try_acquire(2));
        semaphore.release(2);
    }());
}
