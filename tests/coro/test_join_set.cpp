// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_event.h>
#include <dftracer/utils/coro/join_set.h>
#include <dftracer/utils/coro/stage.h>
#include <dftracer/utils/coro/stream.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace std::chrono_literals;

namespace {

CoroTask<int> after(std::chrono::milliseconds delay, int value) {
    co_await sleep(delay);
    co_return value;
}

CoroTask<int> fails_after(std::chrono::milliseconds delay) {
    co_await sleep(delay);
    throw std::runtime_error("planned failure");
}

CoroTask<int> when_set(AsyncEvent& gate, int value) {
    co_await gate.wait();
    co_return value;
}

CoroTask<int> fails_when_set(AsyncEvent& gate) {
    co_await gate.wait();
    throw std::runtime_error("planned failure");
}

AsyncGenerator<int> numbers(int first, int count) {
    for (int i = 0; i < count; ++i) co_yield first + i;
}

AsyncGenerator<int> counted_numbers(std::atomic<int>* produced, int count) {
    for (int i = 0; i < count; ++i) {
        produced->fetch_add(1);
        co_yield i;
    }
}

template <typename T>
CoroTask<dstd::Vector<T>> collect(AsyncGenerator<T> source) {
    dstd::Vector<T> out;
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        out.push_back(std::move(*value));
    }
    co_return out;
}

struct SlowFirst {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "slow_first";
    CoroTask<int> run(int value) const {
        co_await sleep(value == 0 ? 20ms : 1ms);
        co_return value;
    }
};

struct Gauged {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "gauged";
    std::atomic<int>* running;
    std::atomic<int>* peak;
    CoroTask<int> run(int value) const {
        const int now = running->fetch_add(1) + 1;
        int seen = peak->load();
        while (now > seen && !peak->compare_exchange_weak(seen, now)) {
        }
        co_await sleep(1ms);
        running->fetch_sub(1);
        co_return value;
    }
};

struct FailsOnThree {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "fails_on_three";
    CoroTask<int> run(int value) const {
        if (value == 3) {
            throw std::runtime_error("planned failure");
        }
        co_return value;
    }
};

}  // namespace

TEST_CASE("a join set gives results as they finish, with errors as values") {
    Runtime runtime(2);
    AsyncEvent first;
    AsyncEvent second;
    AsyncEvent third;
    auto seen = dftu_utils_test::ok(runtime.block_on(with_join_set<int>(
        [&](JoinSet<int>& set) -> CoroTask<dstd::Vector<int>> {
            set.spawn(when_set(third, 3));
            set.spawn(when_set(first, 1));
            set.spawn(fails_when_set(second));
            CHECK(set.size() == 3);
            dstd::Vector<int> out;
            for (AsyncEvent* gate : {&first, &second, &third}) {
                gate->set();
                auto outcome = co_await set.next();
                REQUIRE(outcome);
                out.push_back(outcome->has_value() ? **outcome : -1);
            }
            auto none = co_await set.next();
            CHECK(!none);
            CHECK(set.size() == 0);
            co_return out;
        })));
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == 1);
    CHECK(seen[1] == -1);
    CHECK(seen[2] == 3);
}

TEST_CASE("a join set wait can time out and lose nothing") {
    Runtime runtime(2);
    auto value = dftu_utils_test::ok(runtime.block_on(
        with_join_set<int>([](JoinSet<int>& set) -> CoroTask<int> {
            set.spawn(after(500ms, 7));
            auto waited = co_await set.next_within(5ms);
            REQUIRE_FALSE(waited);
            CHECK(waited.error().condition() == dstd::Condition::Timeout);
            CHECK(set.size() == 1);
            auto outcome = co_await set.next();
            co_return outcome->value();
        })));
    CHECK(value == 7);
}

TEST_CASE("ending a join set stops the tasks it left running") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    dftu_utils_test::ok(runtime.block_on(
        with_join_set<int>([](JoinSet<int>& set) -> CoroTask<void> {
            set.spawn(after(30s, 1));
            co_await sleep(1ms);
        })));
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("a join set task that a stop ends reports as cancelled") {
    Runtime runtime(2);
    dftu_utils_test::ok(runtime.block_on(
        with_join_set<int>([](JoinSet<int>& set) -> CoroTask<void> {
            set.spawn(after(30s, 1));
            set.request_stop();
            auto outcome = co_await set.next();
            REQUIRE(outcome.has_value());
            REQUIRE_FALSE(outcome->has_value());
            CHECK(outcome->error().condition() == dstd::Condition::Cancelled);
        })));
}

TEST_CASE("as_completed gives each outcome and honors its limit") {
    Runtime runtime(4);
    dstd::Vector<CoroTask<int>> tasks;
    tasks.push_back(after(300ms, 3));
    tasks.push_back(fails_after(1ms));
    tasks.push_back(after(150ms, 2));
    auto outcomes = dftu_utils_test::ok(
        runtime.block_on(collect(as_completed(std::move(tasks)))));
    REQUIRE(outcomes.size() == 3);
    CHECK_FALSE(outcomes[0].has_value());
    CHECK(*outcomes[1] == 2);
    CHECK(*outcomes[2] == 3);

    dstd::Vector<CoroTask<int>> slow;
    slow.push_back(after(30s, 1));
    auto timed = dftu_utils_test::ok(runtime.block_on(
        collect(with_item_timeout(as_completed(std::move(slow)), 10ms))));
    REQUIRE(timed.size() == 1);
    REQUIRE_FALSE(timed[0]);
    CHECK(timed[0].error().condition() == dstd::Condition::Timeout);
}

TEST_CASE("map_ordered keeps input order behind a slow item") {
    Runtime runtime(4);
    constexpr int COUNT = 200;
    auto out = dftu_utils_test::ok(runtime.block_on(collect(map_ordered(
        numbers(0, COUNT),
        [](int value) -> CoroTask<int> {
            co_await sleep(value % 10 == 0 ? 3ms : 0ms);
            co_return value;
        },
        4))));
    REQUIRE(out.size() == COUNT);
    for (int i = 0; i < COUNT; ++i) CHECK(out[i] == i);
    auto plain = dftu_utils_test::ok(runtime.block_on(
        collect(map_ordered(numbers(1, 3), [](int v) { return v * 2; }, 2))));
    REQUIRE(plain.size() == 3);
    CHECK(plain[2] == 6);
}

TEST_CASE("map_ordered stops at its tokens when nobody reads") {
    Runtime runtime(2);
    std::atomic<int> produced{0};
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        auto stream = map_ordered(
            counted_numbers(&produced, 1000), [](int v) { return v; }, 4);
        auto first = co_await stream.next();
        CHECK(first.has_value());
        co_await sleep(50ms);
        CHECK(produced.load() <= 10);
    }()));
    dftu_utils_test::ok(
        runtime.block_on([]() -> CoroTask<void> { co_await sleep(20ms); }()));
    const int after_drop = produced.load();
    dftu_utils_test::ok(
        runtime.block_on([]() -> CoroTask<void> { co_await sleep(20ms); }()));
    CHECK(produced.load() == after_drop);
}

TEST_CASE("map_unordered yields every result") {
    Runtime runtime(4);
    auto out = dftu_utils_test::ok(runtime.block_on(collect(map_unordered(
        numbers(0, 100), [](int v) { return after(1ms, v); }, 8))));
    std::sort(out.begin(), out.end());
    REQUIRE(out.size() == 100);
    CHECK(out[99] == 99);
}

TEST_CASE("ordered and unordered make a stream stage within their tokens") {
    Runtime runtime(4);
    auto kept = SlowFirst{} | ordered(3);
    static_assert(StreamStage<decltype(kept)>);
    CHECK(stage_name(kept) == dstd::StringView("slow_first"));
    auto in_order =
        dftu_utils_test::ok(runtime.block_on(collect(kept.run(numbers(0, 6)))));
    REQUIRE(in_order.size() == 6);
    for (int i = 0; i < 6; ++i) CHECK(in_order[i] == i);

    auto loose = SlowFirst{} | unordered(3);
    static_assert(StreamStage<decltype(loose)>);
    auto first_done = dftu_utils_test::ok(
        runtime.block_on(collect(loose.run(numbers(0, 6)))));
    REQUIRE(first_done.size() == 6);
    CHECK(first_done[0] != 0);

    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    auto gauged = Gauged{&running, &peak} | ordered(2);
    auto done = dftu_utils_test::ok(
        runtime.block_on(collect(gauged.run(numbers(0, 50)))));
    CHECK(done.size() == 50);
    CHECK(peak.load() <= 2);
    CHECK(peak.load() >= 1);
}

TEST_CASE("an ordered stage raises a job error and a move-only stage works") {
    Runtime runtime(4);
    auto failing = FailsOnThree{} | ordered(2);
    dstd::String raised;
    try {
        dftu_utils_test::ok(
            runtime.block_on(collect(failing.run(numbers(0, 100)))));
    } catch (const std::runtime_error& e) {
        raised = e.what();
    }
    CHECK(raised == "planned failure");

    auto erased = erase(SlowFirst{}) | ordered(2);
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(erased.run(numbers(0, 4)))));
    CHECK(out.size() == 4);
}

TEST_CASE("next_within loses no result when a result and the timeout race") {
    Runtime runtime(4);
    constexpr int ROUNDS = 200;
    auto seen = dftu_utils_test::ok(runtime.block_on(
        with_join_set<int>([](JoinSet<int>& set) -> CoroTask<int> {
            int count = 0;
            for (int i = 0; i < ROUNDS; ++i) {
                set.spawn(after(1ms, i));
            }
            while (set.size() != 0) {
                auto outcome = co_await set.next_within(1ms);
                if (outcome && *outcome) ++count;
            }
            co_return count;
        })));
    CHECK(seen == ROUNDS);
}
