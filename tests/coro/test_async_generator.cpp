#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils::coro;

static CoroTask<void> async_delay(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    co_return;
}

static AsyncGenerator<int> async_range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_await async_delay(5);
        co_yield i;
    }
}

static AsyncGenerator<int> async_fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_await async_delay(5);
        co_yield a;
        int next = a + b;
        a = b;
        b = next;
    }
}

static AsyncGenerator<std::string> async_words() {
    co_await async_delay(10);
    co_yield "hello";
    co_await async_delay(10);
    co_yield "async";
    co_await async_delay(10);
    co_yield "world";
}

static CoroTask<std::vector<int>> consume_int_generator(
    AsyncGenerator<int> gen) {
    std::vector<int> result;
    while (auto value = co_await gen.next()) {
        result.push_back(*value);
    }
    co_return result;
}

static CoroTask<std::vector<std::string>> consume_string_generator(
    AsyncGenerator<std::string> gen) {
    std::vector<std::string> result;
    while (auto value = co_await gen.next()) {
        result.push_back(*value);
    }
    co_return result;
}

TEST_CASE("AsyncGenerator - Basic async range generation") {
    auto gen = async_range(0, 5);
    auto task = consume_int_generator(std::move(gen));
    auto result = task.get();

    CHECK(result.size() == 5);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 2);
    CHECK(result[3] == 3);
    CHECK(result[4] == 4);
}

TEST_CASE("AsyncGenerator - Async fibonacci sequence") {
    auto gen = async_fibonacci(7);
    auto task = consume_int_generator(std::move(gen));
    auto result = task.get();

    CHECK(result.size() == 7);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 1);
    CHECK(result[3] == 2);
    CHECK(result[4] == 3);
    CHECK(result[5] == 5);
    CHECK(result[6] == 8);
}

TEST_CASE("AsyncGenerator - String generation with async delays") {
    auto gen = async_words();
    auto task = consume_string_generator(std::move(gen));
    auto result = task.get();

    CHECK(result.size() == 3);
    CHECK(result[0] == "hello");
    CHECK(result[1] == "async");
    CHECK(result[2] == "world");
}

TEST_CASE("AsyncGenerator - Empty generator") {
    auto gen = async_range(5, 5);  // Empty range
    auto task = consume_int_generator(std::move(gen));
    auto result = task.get();

    CHECK(result.empty());
}

TEST_CASE("AsyncGenerator - Move semantics") {
    auto gen1 = async_range(0, 3);
    auto gen2 = std::move(gen1);  // Move construct

    auto task = consume_int_generator(std::move(gen2));
    auto result = task.get();

    CHECK(result.size() == 3);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 2);
}

// Async generator that throws an exception
static AsyncGenerator<int> async_throwing_generator() {
    co_await async_delay(5);
    co_yield 1;
    co_await async_delay(5);
    co_yield 2;
    throw std::runtime_error("Async generator exception");
    co_yield 3;  // Never reached
}

TEST_CASE("AsyncGenerator - Exception propagation") {
    auto gen = async_throwing_generator();

    auto task = [](AsyncGenerator<int> g) -> CoroTask<std::vector<int>> {
        std::vector<int> result;
        try {
            while (auto value = co_await g.next()) {
                result.push_back(*value);
            }
        } catch (const std::runtime_error&) {
            // Exception caught during iteration
            co_return result;
        }
        co_return result;
    }(std::move(gen));

    auto result = task.get();

    // Should have gotten first two values before exception
    CHECK(result.size() == 2);
    CHECK(result[0] == 1);
    CHECK(result[1] == 2);
}

// Test with larger async data
TEST_CASE("AsyncGenerator - Large async sequence") {
    constexpr int N = 100;
    auto gen = async_range(0, N);

    auto task = [](AsyncGenerator<int> g) -> CoroTask<int> {
        int sum = 0;
        while (auto value = co_await g.next()) {
            sum += *value;
        }
        co_return sum;
    }(std::move(gen));

    int sum = task.get();

    CHECK(sum == (N * (N - 1)) / 2);  // Sum formula: n*(n-1)/2
}

// Async generator with struct/class values
struct Point {
    int x, y;

    bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
    }
};

static AsyncGenerator<Point> async_points() {
    co_await async_delay(5);
    co_yield Point{0, 0};
    co_await async_delay(5);
    co_yield Point{1, 1};
    co_await async_delay(5);
    co_yield Point{2, 4};
}

TEST_CASE("AsyncGenerator - Complex types with async operations") {
    auto gen = async_points();

    auto task = [](AsyncGenerator<Point> g) -> CoroTask<std::vector<Point>> {
        std::vector<Point> result;
        while (auto value = co_await g.next()) {
            result.push_back(*value);
        }
        co_return result;
    }(std::move(gen));

    auto result = task.get();

    CHECK(result.size() == 3);
    CHECK(result[0] == (Point{0, 0}));
    CHECK(result[1] == (Point{1, 1}));
    CHECK(result[2] == (Point{2, 4}));
}

TEST_CASE("AsyncGenerator - Manual next() calls") {
    auto gen = async_range(10, 13);

    auto task = [](AsyncGenerator<int> g) -> CoroTask<void> {
        // Manual iteration using next()
        auto val1 = co_await g.next();
        CHECK(val1.has_value());
        CHECK(*val1 == 10);

        auto val2 = co_await g.next();
        CHECK(val2.has_value());
        CHECK(*val2 == 11);

        auto val3 = co_await g.next();
        CHECK(val3.has_value());
        CHECK(*val3 == 12);

        auto val4 = co_await g.next();
        CHECK(!val4.has_value());  // Should be done

        co_return;
    }(std::move(gen));

    task.get();
}

TEST_CASE("AsyncGenerator - done() method") {
    auto gen = async_range(0, 2);

    // Initially not done (coroutine not started)
    CHECK(!gen.done());

    auto task = [](AsyncGenerator<int> g) -> CoroTask<void> {
        co_await g.next();  // 0
        CHECK(!g.done());

        co_await g.next();  // 1
        CHECK(!g.done());

        co_await g.next();  // Should return nullopt
        CHECK(g.done());

        co_return;
    }(std::move(gen));

    task.get();
}
