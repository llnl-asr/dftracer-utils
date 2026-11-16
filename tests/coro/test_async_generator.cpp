// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <chrono>
#include <thread>

using namespace dftracer::utils;

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

static AsyncGenerator<dstd::String> async_words() {
    co_await async_delay(10);
    co_yield "hello";
    co_await async_delay(10);
    co_yield "async";
    co_await async_delay(10);
    co_yield "world";
}

static CoroTask<dstd::Vector<int>> consume_int_generator(
    AsyncGenerator<int> gen) {
    dstd::Vector<int> result;
    while (true) {
        auto value = co_await gen.next();
        if (!value) break;
        result.push_back(*value);
    }
    co_return result;
}

static CoroTask<dstd::Vector<dstd::String>> consume_string_generator(
    AsyncGenerator<dstd::String> gen) {
    dstd::Vector<dstd::String> result;
    while (true) {
        auto value = co_await gen.next();
        if (!value) break;
        result.push_back(*value);
    }
    co_return result;
}

static CoroTask<int> parity(int i) { co_return i & 1; }

static AsyncGenerator<int> ready_range(int n) {
    for (int i = 0; i < n; ++i) co_yield co_await parity(i);
}

static CoroTask<long> sum_ready(int n) {
    long sum = 0;
    for (int i = 0; i < n; ++i) sum += co_await parity(i);
    auto gen = ready_range(n);
    while (true) {
        auto v = co_await gen.next();
        if (!v) break;
        sum += *v;
    }
    co_return sum;
}

// Each await below ends at once. If one added stack frames, as a symmetric
// transfer does when GCC does not make it a tail call, this would overflow
// the stack long before the end.
TEST_CASE("AsyncGenerator - awaits that end at once keep the stack flat") {
    const int n = static_cast<int>(DFTRACER_UTILS_VALGRIND_SCALE(1000000, 10));
    CHECK(dftu_utils_test::ok(sum_ready(n).get()) == n);
}

TEST_CASE("AsyncGenerator - Basic async range generation") {
    auto gen = async_range(0, 5);
    auto task = consume_int_generator(std::move(gen));
    auto result = dftu_utils_test::ok(task.get());

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
    auto result = dftu_utils_test::ok(task.get());

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
    auto result = dftu_utils_test::ok(task.get());

    CHECK(result.size() == 3);
    CHECK(result[0] == "hello");
    CHECK(result[1] == "async");
    CHECK(result[2] == "world");
}

TEST_CASE("AsyncGenerator - Empty generator") {
    auto gen = async_range(5, 5);  // Empty range
    auto task = consume_int_generator(std::move(gen));
    auto result = dftu_utils_test::ok(task.get());

    CHECK(result.empty());
}

TEST_CASE("AsyncGenerator - Move semantics") {
    auto gen1 = async_range(0, 3);
    auto gen2 = std::move(gen1);  // Move construct

    auto task = consume_int_generator(std::move(gen2));
    auto result = dftu_utils_test::ok(task.get());

    CHECK(result.size() == 3);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 2);
}

// Async generator that ends with an error
static AsyncGenerator<int> async_failing_generator() {
    co_await async_delay(5);
    co_yield 1;
    co_await async_delay(5);
    co_yield 2;
    dstd::Error error{dstd::Condition::Unknown, "Async generator error"};
    auto failure = fail_with(std::move(error));
    co_await failure;
    co_yield 3;  // Never reached
}

TEST_CASE("AsyncGenerator - a plain read ends the reader with the error") {
    auto task = [](AsyncGenerator<int> g) -> CoroTask<dstd::Vector<int>> {
        dstd::Vector<int> result;
        while (true) {
            auto value = co_await g.next();
            if (!value) break;
            result.push_back(*value);
        }
        co_return result;
    }(async_failing_generator());

    const auto result = task.get();
    REQUIRE_FALSE(result.has_value());
    CHECK(dstd::String(result.error().message()) == "Async generator error");
}

TEST_CASE("AsyncGenerator - Error propagation") {
    auto gen = async_failing_generator();

    auto task = [](AsyncGenerator<int> g) -> CoroTask<dstd::Vector<int>> {
        dstd::Vector<int> result;
        for (;;) {
            auto next = co_await g.settled_next();
            if (next.error) {
                CHECK(dstd::String(next.error->message()) ==
                      "Async generator error");
                break;
            }
            if (!*next.value) break;
            result.push_back(**next.value);
        }
        co_return result;
    }(std::move(gen));

    auto result = dftu_utils_test::ok(task.get());

    // Should have gotten first two values before exception
    CHECK(result.size() == 2);
    CHECK(result[0] == 1);
    CHECK(result[1] == 2);
}

static AsyncGenerator<int> fail_then_yield(int* ran_after) {
    co_yield 1;
    dstd::Error error{dstd::Condition::Unknown, "generator failed"};
    auto failure = fail_with(std::move(error));
    co_await failure;
    ++*ran_after;
    co_yield 2;
}

TEST_CASE("AsyncGenerator - a read after an error stays failed") {
    int ran_after = 0;
    auto task = [](AsyncGenerator<int> g) -> CoroTask<int> {
        auto first = co_await g.settled_next();
        CHECK(first.value.has_value());
        auto failed = co_await g.settled_next();
        CHECK(failed.error.has_value());
        auto again = co_await g.settled_next();
        CHECK(again.error.has_value());
        CHECK_FALSE(again.value.has_value());
        co_return 0;
    }(fail_then_yield(&ran_after));
    dftu_utils_test::ok(task.get());
    CHECK(ran_after == 0);
}

// Test with larger async data
TEST_CASE("AsyncGenerator - Large async sequence") {
    constexpr int N = 100;
    auto gen = async_range(0, N);

    auto task = [](AsyncGenerator<int> g) -> CoroTask<int> {
        int sum = 0;
        while (true) {
            auto value = co_await g.next();
            if (!value) break;
            sum += *value;
        }
        co_return sum;
    }(std::move(gen));

    int sum = dftu_utils_test::ok(task.get());

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

    auto task = [](AsyncGenerator<Point> g) -> CoroTask<dstd::Vector<Point>> {
        dstd::Vector<Point> result;
        while (true) {
            auto value = co_await g.next();
            if (!value) break;
            result.push_back(*value);
        }
        co_return result;
    }(std::move(gen));

    auto result = dftu_utils_test::ok(task.get());

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

    dftu_utils_test::ok(task.get());
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

    dftu_utils_test::ok(task.get());
}
