// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <chrono>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

// ============================================================================
// CoroTask Combinator Tests
// ============================================================================

TEST_CASE("CoroTask - then() combinator") {
    auto task = []() -> CoroTask<int> { co_return 42; };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 42);
}

TEST_CASE("CoroTask - then() with transformation inside coroutine") {
    auto parent_task = []() -> CoroTask<dstd::String> {
        // Create a coroutine that returns int
        auto compute = []() -> CoroTask<int> { co_return 21; };

        // Use then() to transform the result
        auto transformed = compute().then([](int x) { return x * 2; });

        int result = co_await std::move(transformed);
        co_return dstd::to_string(result);
    };

    Runtime rt(2);
    dstd::String result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == "42");
}

TEST_CASE("CoroTask - operator> for chaining") {
    auto parent_task = []() -> CoroTask<int> {
        auto compute = []() -> CoroTask<int> { co_return 10; };

        // Chain multiple transformations using >>
        auto result = co_await (
            compute() > [](int x) { return x + 5; } >
            [](int x) { return x * 2; });

        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == 30);  // (10 + 5) * 2
}

TEST_CASE("CoroTask - tap() for inspection without transformation") {
    std::atomic<int> inspected_value{0};

    auto parent_task = [&inspected_value]() -> CoroTask<int> {
        auto compute = []() -> CoroTask<int> { co_return 99; };

        // Use tap() to inspect without changing the value
        auto result = co_await (
            compute().tap([&inspected_value](int x) { inspected_value = x; }));

        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == 99);
    CHECK(inspected_value.load() == 99);
}

TEST_CASE("CoroTask - tap() chained with then()") {
    dstd::Vector<int> log;

    auto parent_task = [&log]() -> CoroTask<dstd::String> {
        auto compute = []() -> CoroTask<int> { co_return 5; };

        // Chain tap and then
        auto result =
            co_await (compute()
                          .tap([&log](int x) { log.push_back(x); })
                          .then([](int x) { return x * 10; })
                          .tap([&log](int x) { log.push_back(x); })
                          .then([](int x) { return dstd::to_string(x); }));

        co_return result;
    };

    Runtime rt(2);
    dstd::String result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == "50");
    CHECK(log.size() == 2);
    CHECK(log[0] == 5);
    CHECK(log[1] == 50);
}

TEST_CASE("CoroTask<void> - then() combinator") {
    std::atomic<bool> work_done{false};

    auto parent_task = [&work_done]() -> CoroTask<int> {
        auto do_work = [&work_done]() -> CoroTask<void> {
            work_done = true;
            co_return;
        };

        // Chain void task with value-returning function
        auto result = co_await (do_work().then([]() { return 42; }));

        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == 42);
    CHECK(work_done.load() == true);
}

TEST_CASE("CoroTask<void> - tap() for side effects") {
    std::atomic<int> side_effect_count{0};

    auto parent_task = [&side_effect_count]() -> CoroTask<int> {
        auto do_work = []() -> CoroTask<void> { co_return; };

        // Use tap on void task
        co_await (do_work()
                      .tap([&side_effect_count]() { side_effect_count++; })
                      .tap([&side_effect_count]() { side_effect_count++; }));

        co_return side_effect_count.load();
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == 2);
    CHECK(side_effect_count.load() == 2);
}

TEST_CASE("CoroTask - operator< reverse composition") {
    auto parent_task = []() -> CoroTask<dstd::String> {
        auto compute = []() -> CoroTask<int> { co_return 7; };

        // Reverse composition: func < task
        auto result =
            co_await ([](int x) { return dstd::to_string(x * 3); } < compute());

        co_return result;
    };

    Runtime rt(2);
    dstd::String result = dftu_utils_test::ok(rt.block_on(parent_task()));
    CHECK(result == "21");
}

// ============================================================================
// Mixed Combinator Tests
// ============================================================================

TEST_CASE("Mixed - CoroTask combinators with side effects") {
    std::atomic<int> checkpoint{0};

    auto task = [&checkpoint]() -> CoroTask<int> {
        checkpoint = 1;

        // Use CoroTask combinators inside a task
        auto compute = []() -> CoroTask<int> { co_return 15; };

        int result = co_await (compute()
                                   .tap([&checkpoint](int) { checkpoint = 2; })
                                   .then([](int x) { return x + 5; })
                                   .tap([&checkpoint](int) { checkpoint = 3; })
                                   .then([](int x) { return x * 2; }));

        checkpoint = 4;
        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 40);  // (15 + 5) * 2
    CHECK(checkpoint.load() == 4);
}

// ============================================================================
// Performance and Stress Tests
// ============================================================================

TEST_CASE("Performance - Long chain of then() operations") {
    auto task = []() -> CoroTask<int> {
        auto compute = []() -> CoroTask<int> { co_return 1; };

        // Long chain
        auto result = co_await (compute()
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; })
                                    .then([](int x) { return x + 1; }));

        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 11);  // 1 + 10
}

TEST_CASE("Performance - Multiple tap() operations") {
    std::atomic<int> tap_count{0};

    auto task = [&tap_count]() -> CoroTask<int> {
        auto compute = []() -> CoroTask<int> { co_return 42; };

        auto result = co_await (compute()
                                    .tap([&tap_count](int) { tap_count++; })
                                    .tap([&tap_count](int) { tap_count++; })
                                    .tap([&tap_count](int) { tap_count++; })
                                    .tap([&tap_count](int) { tap_count++; })
                                    .tap([&tap_count](int) { tap_count++; }));

        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 42);
    CHECK(tap_count.load() == 5);
}

// ============================================================================
// Operator& (AND/Parallel) Tests
// ============================================================================

TEST_CASE("CoroTask - operator& parallel composition") {
    auto task = []() -> CoroTask<int> {
        auto compute1 = []() -> CoroTask<int> { co_return 10; };

        auto compute2 = []() -> CoroTask<dstd::String> {
            co_return dstd::String("hello");
        };

        // Use operator& to combine both
        auto [val1, val2] = co_await (compute1() & compute2());

        co_return val1 + static_cast<int>(val2.length());
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 15);  // 10 + 5 (length of "hello")
}

TEST_CASE("CoroTask<void> - operator& with value task") {
    std::atomic<bool> side_effect{false};

    auto task = [&side_effect]() -> CoroTask<int> {
        auto void_work = [&side_effect]() -> CoroTask<void> {
            side_effect = true;
            co_return;
        };

        auto compute = []() -> CoroTask<int> { co_return 42; };

        // Combine void task with value task
        int result = co_await (void_work() & compute());
        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 42);
    CHECK(side_effect.load() == true);
}

// ============================================================================
// Operator| (OR/Fallback) Tests
// ============================================================================

TEST_CASE("CoroTask - operator| fallback on success") {
    auto task = []() -> CoroTask<int> {
        auto primary = []() -> CoroTask<int> { co_return 99; };

        auto fallback = []() -> CoroTask<int> { co_return 0; };

        // Primary succeeds, fallback not executed
        int result = co_await primary().or_else(fallback());
        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 99);
}

TEST_CASE("CoroTask - operator| fallback on failure") {
    auto task = []() -> CoroTask<int> {
        auto primary = []() -> CoroTask<int> {
            throw std::runtime_error("Primary failed");
            co_return 0;
        };

        auto fallback = []() -> CoroTask<int> { co_return 42; };

        // Primary fails, fallback executes
        int result = co_await primary().or_else(fallback());
        co_return result;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 42);
}

TEST_CASE("CoroTask<void> - operator| fallback") {
    std::atomic<int> execution_count{0};

    auto task = [&execution_count]() -> CoroTask<void> {
        auto primary = [&execution_count]() -> CoroTask<void> {
            execution_count++;
            throw std::runtime_error("Primary void task failed");
            co_return;
        };

        auto fallback = [&execution_count]() -> CoroTask<void> {
            execution_count++;
            co_return;
        };

        // Primary fails, fallback executes
        co_await primary().or_else(fallback());
    };

    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on(task()));

    CHECK(execution_count.load() == 2);  // Both executed
}

// ============================================================================
// Combined Operators Tests
// ============================================================================

TEST_CASE("CoroTask - Combining & and | operators") {
    auto task = []() -> CoroTask<int> {
        auto compute1 = []() -> CoroTask<int> { co_return 10; };

        auto compute2_primary = []() -> CoroTask<int> {
            throw std::runtime_error("Compute2 failed");
            co_return 0;
        };

        auto compute2_fallback = []() -> CoroTask<int> { co_return 5; };

        // Combine: compute1 AND (compute2_primary OR compute2_fallback)
        auto [val1, val2] = co_await (
            compute1() & compute2_primary().or_else(compute2_fallback()));

        co_return val1 + val2;
    };

    Runtime rt(2);
    int result = dftu_utils_test::ok(rt.block_on(task()));
    CHECK(result == 15);  // 10 + 5 (fallback)
}
