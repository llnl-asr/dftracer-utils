// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/stream.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace std::chrono_literals;

namespace {

AsyncGenerator<int> numbers(int first, int count) {
    for (int i = 0; i < count; ++i) co_yield first + i;
}

AsyncGenerator<int> slow_numbers(int first, int count,
                                 std::chrono::milliseconds delay) {
    for (int i = 0; i < count; ++i) {
        co_await sleep(delay);
        co_yield first + i;
    }
}

AsyncGenerator<int> fails_after(int count) {
    for (int i = 0; i < count; ++i) co_yield i;
    throw common::DFTUtilsException(
        dstd::Error{dstd::Condition::Io, "planned failure"});
}

AsyncGenerator<int> counted_numbers(std::atomic<int>* produced) {
    for (int i = 0; i < 100000; ++i) {
        co_await sleep(1ms);
        produced->fetch_add(1);
        co_yield i;
    }
}

AsyncGenerator<int> one_then_fails(std::chrono::milliseconds delay) {
    co_yield 0;
    co_await sleep(delay);
    throw common::DFTUtilsException(
        dstd::Error{dstd::Condition::Io, "planned failure"});
}

AsyncGenerator<int> reports_closure(dstd::WeakPtr<int> closure,
                                    std::atomic<bool>* gone) {
    co_await sleep(30ms);
    gone->store(closure.expired());
    co_yield 1;
}

CoroTask<void> settle() { co_await sleep(50ms); }

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

CoroTask<int> doubled_later(int value) {
    co_await sleep(1ms);
    co_return value * 2;
}

}  // namespace

TEST_CASE("map takes a function that returns a value or a task") {
    Runtime runtime(2);
    auto plain = dftu_utils_test::ok(runtime.block_on(
        collect(map(numbers(1, 3), [](int v) { return v * 10; }))));
    REQUIRE(plain.size() == 3);
    CHECK(plain[2] == 30);
    auto awaited = dftu_utils_test::ok(
        runtime.block_on(collect(map(numbers(1, 3), doubled_later))));
    REQUIRE(awaited.size() == 3);
    CHECK(awaited[2] == 6);
}

TEST_CASE("take_until stops before the matching item") {
    Runtime runtime(2);
    auto out = dftu_utils_test::ok(runtime.block_on(
        collect(take_until(numbers(0, 100), [](int v) { return v == 3; }))));
    REQUIRE(out.size() == 3);
    CHECK(out[2] == 2);
}

TEST_CASE("chunks keeps every item and yields a short last chunk") {
    Runtime runtime(2);
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(chunks(numbers(0, 7), 3))));
    REQUIRE(out.size() == 3);
    CHECK(out[0].size() == 3);
    CHECK(out[1].size() == 3);
    REQUIRE(out[2].size() == 1);
    CHECK(out[2][0] == 6);
    auto exact = dftu_utils_test::ok(
        runtime.block_on(collect(chunks(numbers(0, 6), 3))));
    CHECK(exact.size() == 2);
}

TEST_CASE("zip ends with the shorter source") {
    Runtime runtime(2);
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(zip(numbers(0, 3), numbers(10, 100)))));
    REQUIRE(out.size() == 3);
    CHECK(out[2].first == 2);
    CHECK(out[2].second == 12);
}

TEST_CASE("fold combines every item") {
    Runtime runtime(2);
    const int sum = dftu_utils_test::ok(runtime.block_on(
        fold(numbers(1, 4), 0, [](int acc, int v) { return acc + v; })));
    CHECK(sum == 10);
}

TEST_CASE("merge yields every item and keeps the order of one source") {
    Runtime runtime(4);
    dstd::Vector<AsyncGenerator<int>> sources;
    constexpr int PER_SOURCE = 2000;
    for (int s = 0; s < 4; ++s) {
        sources.push_back(numbers(s * 100000, PER_SOURCE));
    }
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(merge(std::move(sources)))));
    REQUIRE(out.size() == 4 * PER_SOURCE);
    int next[4] = {0, 0, 0, 0};
    for (int value : out) {
        const int source = value / 100000;
        CHECK(value % 100000 == next[source]);
        ++next[source];
    }
    auto none = dftu_utils_test::ok(
        runtime.block_on(collect(merge(dstd::Vector<AsyncGenerator<int>>{}))));
    CHECK(none.empty());
}

TEST_CASE("merge raises a source error from the next read") {
    Runtime runtime(2);
    dstd::Vector<AsyncGenerator<int>> sources;
    sources.push_back(fails_after(2));
    sources.push_back(numbers(0, 2));
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        dftu_utils_test::ok(
            runtime.block_on(collect(merge(std::move(sources)))));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Io);
}

TEST_CASE("buffer keeps order, reads ahead and survives an early drop") {
    Runtime runtime(4);
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(buffer(numbers(0, 5000), 8))));
    REQUIRE(out.size() == 5000);
    for (int i = 0; i < 5000; ++i) CHECK(out[i] == i);

    std::atomic<int> first{-1};
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        auto stream = buffer(slow_numbers(5, 1000, 1ms), 2);
        auto value = co_await stream.next();
        first.store(*value);
    }()));
    CHECK(first.load() == 5);
    dftu_utils_test::ok(
        runtime.block_on([]() -> CoroTask<void> { co_await sleep(20ms); }()));
}

TEST_CASE("flat_map_merge yields every item within its limit") {
    Runtime runtime(4);
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    auto inner = [&running, &peak](int value) -> AsyncGenerator<int> {
        const int now = running.fetch_add(1) + 1;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        for (int i = 0; i < 10; ++i) {
            co_await sleep(1ms);
            co_yield value * 100 + i;
        }
        running.fetch_sub(1);
    };
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(flat_map_merge(numbers(0, 12), inner, 3))));
    REQUIRE(out.size() == 120);
    std::sort(out.begin(), out.end());
    for (std::size_t i = 1; i < out.size(); ++i) CHECK(out[i - 1] < out[i]);
    CHECK(peak.load() <= 3);
    CHECK(peak.load() >= 2);
    CHECK(running.load() == 0);
}

TEST_CASE("flat_map_merge raises an inner error and takes an empty source") {
    Runtime runtime(2);
    auto failing = [](int) { return fails_after(1); };
    CHECK_THROWS_AS(dftu_utils_test::ok(runtime.block_on(
                        collect(flat_map_merge(numbers(0, 4), failing, 2)))),
                    common::DFTUtilsException);
    auto empty_source = [](int v) { return numbers(v, 1); };
    auto none = dftu_utils_test::ok(runtime.block_on(
        collect(flat_map_merge(numbers(0, 0), empty_source, 2))));
    CHECK(none.empty());
}

TEST_CASE("flat_map_merge keeps its function alive for the last generators") {
    Runtime runtime(2);
    std::atomic<bool> gone{true};
    auto token = dstd::make_shared<int>(1);
    dstd::WeakPtr<int> weak = token;
    auto fn = [token = std::move(token), weak, &gone](int) {
        return reports_closure(weak, &gone);
    };
    auto out = dftu_utils_test::ok(runtime.block_on(
        collect(flat_map_merge(numbers(0, 1), std::move(fn), 1))));
    CHECK(out.size() == 1);
    CHECK_FALSE(gone.load());
}

TEST_CASE("flat_map_merge reports an outer error when the channel is full") {
    Runtime runtime(2);
    auto inner = [](int v) { return numbers(v, 3); };
    dstd::Condition condition = dstd::Condition::Unknown;
    int seen = 0;
    try {
        dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
            auto stream = flat_map_merge(one_then_fails(5ms), inner, 1);
            auto first = co_await stream.next();
            if (first) ++seen;
            co_await sleep(50ms);
            for (;;) {
                auto value = co_await stream.next();
                if (!value) break;
                ++seen;
            }
        }()));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(seen == 3);
    CHECK(condition == dstd::Condition::Io);
}

TEST_CASE("a dropped stream stops its background sources") {
    Runtime runtime(2);
    auto settles = [&](std::atomic<int>& produced) {
        dftu_utils_test::ok(runtime.block_on(settle()));
        const int after_drop = produced.load();
        dftu_utils_test::ok(runtime.block_on(settle()));
        CHECK(produced.load() == after_drop);
    };

    std::atomic<int> in_buffer{0};
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        auto stream = buffer(counted_numbers(&in_buffer), 2);
        auto value = co_await stream.next();
        CHECK(value.has_value());
    }()));
    settles(in_buffer);

    std::atomic<int> in_merge{0};
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        dstd::Vector<AsyncGenerator<int>> sources;
        sources.push_back(counted_numbers(&in_merge));
        auto stream = merge(std::move(sources));
        auto value = co_await stream.next();
        CHECK(value.has_value());
    }()));
    settles(in_merge);

    std::atomic<int> in_inner{0};
    auto inner = [&in_inner](int) { return counted_numbers(&in_inner); };
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        auto stream = flat_map_merge(numbers(0, 100), inner, 2);
        auto value = co_await stream.next();
        CHECK(value.has_value());
    }()));
    settles(in_inner);
}

TEST_CASE("a timeout ends a wait inside a generator") {
    Runtime runtime(2);
    const auto start = std::chrono::steady_clock::now();
    auto result =
        runtime.block_on(with_timeout(collect(slow_numbers(0, 3, 30s)), 10ms));
    REQUIRE_FALSE(result);
    CHECK(result.error().condition() == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("a cancel reaches through generators and a task they await") {
    Runtime runtime(2);
    auto waits = []() -> CoroTask<int> {
        co_await sleep(30s);
        co_return 1;
    };
    const auto start = std::chrono::steady_clock::now();
    auto result = runtime.block_on(with_timeout(
        collect(numbers(0, 1) | map([waits](int) { return waits(); }) |
                filter([](int) { return true; })),
        10ms));
    REQUIRE_FALSE(result);
    CHECK(result.error().condition() == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("with_item_timeout gives items in time and ends a slow wait") {
    Runtime runtime(2);
    auto fast = dftu_utils_test::ok(
        runtime.block_on(collect(with_item_timeout(numbers(0, 100), 5s))));
    CHECK(fast.size() == 100);

    const auto start = std::chrono::steady_clock::now();
    auto items = dftu_utils_test::ok(runtime.block_on(
        collect(with_item_timeout(slow_numbers(0, 3, 30s), 10ms))));
    REQUIRE_FALSE(items.empty());
    REQUIRE_FALSE(items.back());
    CHECK(items.back().error().condition() == dstd::Condition::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("a dropped stream ends a source blocked in a long wait") {
    const auto start = std::chrono::steady_clock::now();
    {
        Runtime runtime(2);
        dftu_utils_test::ok(runtime.block_on([]() -> CoroTask<void> {
            dstd::Vector<AsyncGenerator<int>> sources;
            sources.push_back(numbers(1, 1));
            sources.push_back(slow_numbers(0, 3, 30s));
            auto stream = merge(std::move(sources));
            auto first = co_await stream.next();
            CHECK(first.has_value());
        }()));
        dftu_utils_test::ok(runtime.block_on([]() -> CoroTask<void> {
            auto stream =
                buffer(concat(numbers(1, 1), slow_numbers(0, 3, 30s)), 2);
            auto first = co_await stream.next();
            CHECK(first.has_value());
        }()));
    }
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("a generator can ask for its executor and for a stop") {
    Runtime runtime(2);
    auto asks = []() -> AsyncGenerator<int> {
        Executor* executor = co_await this_coro::executor;
        const bool stopped = co_await this_coro::stop_requested;
        co_yield executor != nullptr ? 1 : 0;
        co_yield stopped ? 1 : 0;
    };
    auto out = dftu_utils_test::ok(runtime.block_on(collect(asks())));
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 1);
    CHECK(out[1] == 0);
}

TEST_CASE("functions that return a task work in every operation") {
    Runtime runtime(2);
    auto evens = [](const int& v) -> CoroTask<bool> {
        co_await sleep(0ms);
        co_return v % 2 == 0;
    };
    auto kept = dftu_utils_test::ok(
        runtime.block_on(collect(filter(numbers(0, 6), evens))));
    REQUIRE(kept.size() == 3);
    CHECK(kept[2] == 4);

    auto is_three = [](const int& v) -> CoroTask<bool> { co_return v == 3; };
    auto until = dftu_utils_test::ok(
        runtime.block_on(collect(take_until(numbers(0, 9), is_three))));
    CHECK(until.size() == 3);

    auto add = [](int acc, int v) -> CoroTask<int> { co_return acc + v; };
    CHECK(dftu_utils_test::ok(runtime.block_on(fold(numbers(1, 4), 0, add))) ==
          10);

    auto later = [](int v) -> CoroTask<AsyncGenerator<int>> {
        co_return numbers(v, 2);
    };
    auto flat = dftu_utils_test::ok(
        runtime.block_on(collect(flat_map(numbers(10, 2), later))));
    REQUIRE(flat.size() == 4);
    CHECK(flat[3] == 12);
    auto merged = dftu_utils_test::ok(
        runtime.block_on(collect(flat_map_merge(numbers(10, 2), later, 2))));
    CHECK(merged.size() == 4);
}

TEST_CASE("each stream operation works as an adaptor") {
    Runtime runtime(2);
    auto out = dftu_utils_test::ok(runtime.block_on(
        collect(numbers(0, 20) | map([](int v) { return v + 1; }) |
                filter([](int v) { return v % 2 == 0; }) |
                take_until([](int v) { return v > 12; }) | take(5) | buffer(2) |
                flat_map([](int v) { return numbers(v, 1); }) |
                flat_map_merge([](int v) { return numbers(v, 1); }, 1))));
    REQUIRE(out.size() == 5);
    CHECK(out[4] == 10);
    auto grouped = dftu_utils_test::ok(
        runtime.block_on(collect(numbers(0, 5) | chunks(2))));
    CHECK(grouped.size() == 3);
    auto joined = dftu_utils_test::ok(runtime.block_on(
        collect(numbers(0, 1) | concat([]() { return numbers(5, 1); }))));
    REQUIRE(joined.size() == 2);
    CHECK(joined[1] == 5);
    CHECK(dftu_utils_test::ok(runtime.block_on(
              numbers(1, 4) | fold(0, [](int a, int v) { return a + v; }))) ==
          10);
    auto pair = dftu_utils_test::ok(
        runtime.block_on(collect(concat(numbers(0, 1), numbers(9, 1)))));
    CHECK(pair[1] == 9);
}

TEST_CASE("a generator read by a task can move to a pump after the task ends") {
    Runtime runtime(2);
    auto source = slow_numbers(0, 3, 1ms);
    auto first = dftu_utils_test::ok(
        runtime.block_on([](AsyncGenerator<int>& gen) -> CoroTask<int> {
            auto value = co_await gen.next();
            co_return *value;
        }(source)));
    CHECK(first == 0);
    auto rest = dftu_utils_test::ok(
        runtime.block_on(collect(buffer(std::move(source), 2))));
    REQUIRE(rest.size() == 2);
    CHECK(rest[1] == 2);
}
