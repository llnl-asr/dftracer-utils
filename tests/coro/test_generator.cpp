// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/generator.h>
#include <dftracer/utils/coro/stage.h>
#include <dftracer/utils/coro/stream.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

using namespace dftracer::utils::coro;

// Simple generator that yields a sequence of numbers
static Generator<int> range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;
    }
}

// Generator that yields fibonacci sequence
static Generator<int> fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        int next = a + b;
        a = b;
        b = next;
    }
}

// Generator that yields strings
static Generator<dstd::String> words() {
    co_yield "hello";
    co_yield "world";
    co_yield "coroutines";
}

TEST_CASE("Generator - Basic range generation") {
    dstd::Vector<int> result;
    for (int value : range(0, 5)) {
        result.push_back(value);
    }

    CHECK(result.size() == 5);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 2);
    CHECK(result[3] == 3);
    CHECK(result[4] == 4);
}

TEST_CASE("Generator - Fibonacci sequence") {
    dstd::Vector<int> result;
    for (int value : fibonacci(7)) {
        result.push_back(value);
    }

    CHECK(result.size() == 7);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 1);
    CHECK(result[3] == 2);
    CHECK(result[4] == 3);
    CHECK(result[5] == 5);
    CHECK(result[6] == 8);
}

TEST_CASE("Generator - String generation") {
    dstd::Vector<dstd::String> result;
    for (const auto& word : words()) {
        result.push_back(word);
    }

    CHECK(result.size() == 3);
    CHECK(result[0] == "hello");
    CHECK(result[1] == "world");
    CHECK(result[2] == "coroutines");
}

TEST_CASE("Generator - Manual iteration") {
    auto gen = range(10, 15);

    // Start iteration
    auto it = gen.begin();
    auto end_it = gen.end();

    CHECK(it != end_it);
    CHECK(*it == 10);

    // Advance
    ++it;
    CHECK(*it == 11);

    ++it;
    CHECK(*it == 12);

    ++it;
    CHECK(*it == 13);

    ++it;
    CHECK(*it == 14);

    ++it;
    CHECK(it == end_it);  // Should be done
}

TEST_CASE("Generator - Empty generator") {
    auto gen = range(5, 5);  // Empty range

    dstd::Vector<int> result;
    for (int value : gen) {
        result.push_back(value);
    }

    CHECK(result.empty());
}

TEST_CASE("Generator - Move semantics") {
    auto gen1 = range(0, 3);
    auto gen2 = std::move(gen1);  // Move construct

    dstd::Vector<int> result;
    for (int value : gen2) {
        result.push_back(value);
    }

    CHECK(result.size() == 3);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 2);
}

// Generator that throws an exception
static Generator<int> throwing_generator() {
    co_yield 1;
    co_yield 2;
    throw std::runtime_error("Test exception");
    co_yield 3;  // Never reached
}

TEST_CASE("Generator - Exception propagation") {
    auto gen = throwing_generator();

    dstd::Vector<int> result;
    CHECK_THROWS_AS(
        {
            for (int value : gen) {
                result.push_back(value);
            }
        },
        std::runtime_error);

    // Should have gotten first two values before exception
    CHECK(result.size() == 2);
    CHECK(result[0] == 1);
    CHECK(result[1] == 2);
}

// Test with larger data
TEST_CASE("Generator - Large sequence") {
    constexpr int N = 10000;
    auto gen = range(0, N);

    int count = 0;
    int sum = 0;
    for (int value : gen) {
        sum += value;
        ++count;
    }

    CHECK(count == N);
    CHECK(sum == (N * (N - 1)) / 2);  // Sum formula: n*(n-1)/2
}

// Generator with struct/class values
struct Point {
    int x, y;

    bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
    }
};

static Generator<Point> points() {
    co_yield Point{0, 0};
    co_yield Point{1, 1};
    co_yield Point{2, 4};
}

TEST_CASE("Generator - Complex types") {
    dstd::Vector<Point> result;
    for (const auto& p : points()) {
        result.push_back(p);
    }

    CHECK(result.size() == 3);
    CHECK(result[0] == (Point{0, 0}));
    CHECK(result[1] == (Point{1, 1}));
    CHECK(result[2] == (Point{2, 4}));
}

namespace {

Generator<int> sync_numbers(int first, int count) {
    for (int i = 0; i < count; ++i) co_yield first + i;
}

Generator<int> sync_fails_after(int count) {
    for (int i = 0; i < count; ++i) co_yield i;
    throw common::DFTUtilsException(
        dstd::Error{dstd::Condition::Io, "planned failure"});
}

Generator<dstd::UniquePtr<int>> owned_numbers(int count) {
    for (int i = 0; i < count; ++i) co_yield dstd::make_unique<int>(i);
}

template <typename T>
dstd::Vector<T> drain(Generator<T> source) {
    dstd::Vector<T> out;
    for (;;) {
        auto value = source.next();
        if (!value) break;
        out.push_back(std::move(*value));
    }
    return out;
}

}  // namespace

TEST_CASE("Generator - every stream operation as a function") {
    auto mapped = drain(map(sync_numbers(1, 3), [](int v) { return v * 2; }));
    REQUIRE(mapped.size() == 3);
    CHECK(mapped[2] == 6);

    auto flat = drain(flat_map(sync_numbers(10, 2),
                               [](int v) { return sync_numbers(v, 2); }));
    REQUIRE(flat.size() == 4);
    CHECK(flat[3] == 12);

    auto kept = drain(
        filter(sync_numbers(0, 6), [](const int& v) { return v % 2 == 0; }));
    REQUIRE(kept.size() == 3);
    CHECK(kept[2] == 4);

    CHECK(drain(take(sync_numbers(0, 9), 2)).size() == 2);
    CHECK(drain(take_until(sync_numbers(0, 9), [](const int& v) {
              return v == 3;
          })).size() == 3);

    auto both = drain(concat(sync_numbers(0, 1), sync_numbers(5, 1)));
    REQUIRE(both.size() == 2);
    CHECK(both[1] == 5);
    auto later =
        drain(concat(sync_numbers(0, 1), []() { return sync_numbers(7, 1); }));
    REQUIRE(later.size() == 2);
    CHECK(later[1] == 7);

    auto grouped = drain(chunks(sync_numbers(0, 5), 2));
    REQUIRE(grouped.size() == 3);
    CHECK(grouped[2].size() == 1);
    CHECK_THROWS_AS(drain(chunks(sync_numbers(0, 1), 0)),
                    common::DFTUtilsException);

    auto pairs = drain(zip(sync_numbers(0, 2), sync_numbers(10, 9)));
    REQUIRE(pairs.size() == 2);
    CHECK(pairs[1].second == 11);

    CHECK(fold(sync_numbers(1, 4), 0, [](int a, int v) { return a + v; }) ==
          10);
}

TEST_CASE("Generator - the adaptors of AsyncGenerator apply") {
    auto out = drain(sync_numbers(0, 20) | map([](int v) { return v + 1; }) |
                     filter([](const int& v) { return v % 2 == 0; }) |
                     take_until([](const int& v) { return v > 12; }) | take(5) |
                     flat_map([](int v) { return sync_numbers(v, 1); }) |
                     then([](int v) { return v * 10; }));
    REQUIRE(out.size() == 5);
    CHECK(out[4] == 100);
    CHECK(drain(sync_numbers(0, 5) | chunks(2)).size() == 3);
    CHECK(
        drain(sync_numbers(0, 1) | concat([]() { return sync_numbers(5, 1); }))
            .size() == 2);
    CHECK((sync_numbers(1, 4) | fold(0, [](int a, int v) { return a + v; })) ==
          10);
}

TEST_CASE("Generator - next moves a move-only item out") {
    auto out = drain(map(owned_numbers(3), [](dstd::UniquePtr<int> v) {
        *v += 1;
        return v;
    }));
    REQUIRE(out.size() == 3);
    CHECK(*out[2] == 3);
}

TEST_CASE("Generator - an error reaches the reader through an operation") {
    auto stream = map(sync_fails_after(1), [](int v) { return v; });
    CHECK(stream.next().has_value());
    CHECK_THROWS_AS(stream.next(), common::DFTUtilsException);
}

TEST_CASE("Generator - to_async gives the operations that need an executor") {
    Runtime runtime(4);
    auto collect =
        [](AsyncGenerator<int> source) -> CoroTask<dstd::Vector<int>> {
        dstd::Vector<int> out;
        for (;;) {
            auto value = co_await source.next();
            if (!value) break;
            out.push_back(*value);
        }
        co_return out;
    };
    auto ordered =
        runtime.block_on(collect(to_async(sync_numbers(0, 50)) |
                                 map_ordered([](int v) { return v * 2; }, 4)));
    REQUIRE(ordered.size() == 50);
    CHECK(ordered[49] == 98);
    auto loose =
        runtime.block_on(collect(to_async(sync_numbers(0, 50)) |
                                 map_unordered([](int v) { return v; }, 4)));
    CHECK(loose.size() == 50);
    CHECK_THROWS_AS(runtime.block_on(collect(to_async(sync_fails_after(1)))),
                    common::DFTUtilsException);
}
