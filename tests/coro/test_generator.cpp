#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/generator.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

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
static Generator<std::string> words() {
    co_yield "hello";
    co_yield "world";
    co_yield "coroutines";
}

TEST_CASE("Generator - Basic range generation") {
    std::vector<int> result;
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
    std::vector<int> result;
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
    std::vector<std::string> result;
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

    std::vector<int> result;
    for (int value : gen) {
        result.push_back(value);
    }

    CHECK(result.empty());
}

TEST_CASE("Generator - Move semantics") {
    auto gen1 = range(0, 3);
    auto gen2 = std::move(gen1);  // Move construct

    std::vector<int> result;
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

    std::vector<int> result;
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
    std::vector<Point> result;
    for (const auto& p : points()) {
        result.push_back(p);
    }

    CHECK(result.size() == 3);
    CHECK(result[0] == (Point{0, 0}));
    CHECK(result[1] == (Point{1, 1}));
    CHECK(result[2] == (Point{2, 4}));
}
