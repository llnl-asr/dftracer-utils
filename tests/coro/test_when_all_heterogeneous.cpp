// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <stdexcept>
#include <tuple>
#include <variant>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

TEST_CASE("when_all - Two heterogeneous types (int, string)") {
    Runtime rt(4);

    std::atomic<int> int_result{0};
    std::atomic<bool> str_ok{false};

    auto* int_ptr = &int_result;
    auto* str_ptr = &str_ok;

    dftu_utils_test::ok(rt.block_on(coro::with_scope(
        [int_ptr, str_ptr](coro::Scope& scope) -> CoroTask<void> {
            auto f1 =
                scope.spawn_future([]() -> CoroTask<int> { co_return 42; });
            auto f2 = scope.spawn_future([]() -> CoroTask<dstd::String> {
                co_return dstd::String("hello");
            });

            auto [a, b] = co_await when_all(std::move(f1), std::move(f2));

            int_ptr->store(a, std::memory_order_relaxed);
            str_ptr->store(b == "hello", std::memory_order_relaxed);
            co_return;
        })));

    CHECK(int_result.load() == 42);
    CHECK(str_ok.load() == true);
}

TEST_CASE("when_all - Three heterogeneous types (int, float, string)") {
    Runtime rt(4);

    std::atomic<int> int_result{0};
    std::atomic<bool> float_ok{false};
    std::atomic<bool> str_ok{false};

    auto* int_ptr = &int_result;
    auto* float_ptr = &float_ok;
    auto* str_ptr = &str_ok;

    dftu_utils_test::ok(rt.block_on(coro::with_scope(
        [int_ptr, float_ptr, str_ptr](coro::Scope& scope) -> CoroTask<void> {
            auto f1 =
                scope.spawn_future([]() -> CoroTask<int> { co_return 1; });
            auto f2 =
                scope.spawn_future([]() -> CoroTask<float> { co_return 2.5f; });
            auto f3 = scope.spawn_future([]() -> CoroTask<dstd::String> {
                co_return dstd::String("three");
            });

            auto [a, b, c] =
                co_await when_all(std::move(f1), std::move(f2), std::move(f3));

            int_ptr->store(a, std::memory_order_relaxed);
            float_ptr->store(b == 2.5f, std::memory_order_relaxed);
            str_ptr->store(c == "three", std::memory_order_relaxed);
            co_return;
        })));

    CHECK(int_result.load() == 1);
    CHECK(float_ok.load() == true);
    CHECK(str_ok.load() == true);
}

TEST_CASE("when_all - Mixed void and non-void") {
    Runtime rt(4);

    std::atomic<bool> flag{false};
    std::atomic<int> int_result{0};

    auto* flag_ptr = &flag;
    auto* int_ptr = &int_result;

    dftu_utils_test::ok(rt.block_on(coro::with_scope(
        [flag_ptr, int_ptr](coro::Scope& scope) -> CoroTask<void> {
            auto f1 = scope.spawn_future([flag_ptr]() -> CoroTask<void> {
                flag_ptr->store(true, std::memory_order_relaxed);
                co_return;
            });
            auto f2 =
                scope.spawn_future([]() -> CoroTask<int> { co_return 99; });

            auto [mono, val] = co_await when_all(std::move(f1), std::move(f2));

            // mono is dstd::Monostate -- verify it compiles and holds
            static_assert(std::is_same_v<decltype(mono), dstd::Monostate>);
            int_ptr->store(val, std::memory_order_relaxed);
            co_return;
        })));

    CHECK(flag.load() == true);
    CHECK(int_result.load() == 99);
}

TEST_CASE("when_all - All void types") {
    Runtime rt(4);

    std::atomic<int> counter{0};
    auto* ctr = &counter;

    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([ctr](coro::Scope& scope) -> CoroTask<void> {
            auto f1 = scope.spawn_future([ctr]() -> CoroTask<void> {
                ctr->fetch_add(1, std::memory_order_relaxed);
                co_return;
            });
            auto f2 = scope.spawn_future([ctr]() -> CoroTask<void> {
                ctr->fetch_add(1, std::memory_order_relaxed);
                co_return;
            });

            auto [m1, m2] = co_await when_all(std::move(f1), std::move(f2));

            static_assert(std::is_same_v<decltype(m1), dstd::Monostate>);
            static_assert(std::is_same_v<decltype(m2), dstd::Monostate>);
            co_return;
        })));

    CHECK(counter.load() == 2);
}

TEST_CASE("when_all - Exception propagation from one task") {
    Runtime rt(4);

    std::atomic<bool> exception_caught{false};
    auto* caught_ptr = &exception_caught;

    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([caught_ptr](coro::Scope& scope) -> CoroTask<void> {
            auto f1 = scope.spawn_future([]() -> CoroTask<void> {
                throw std::runtime_error("boom");
                co_return;
            });
            auto f2 =
                scope.spawn_future([]() -> CoroTask<int> { co_return 7; });

            try {
                co_await when_all(std::move(f1), std::move(f2));
            } catch (const std::runtime_error& e) {
                caught_ptr->store(dstd::String(e.what()) == "boom",
                                  std::memory_order_relaxed);
            }
            co_return;
        })));

    CHECK(exception_caught.load() == true);
}

TEST_CASE("when_all - Homogeneous types still work with variadic overload") {
    Runtime rt(4);

    std::atomic<int> sum{0};
    auto* sum_ptr = &sum;

    dftu_utils_test::ok(rt.block_on(
        coro::with_scope([sum_ptr](coro::Scope& scope) -> CoroTask<void> {
            auto f1 =
                scope.spawn_future([]() -> CoroTask<int> { co_return 17; });
            auto f2 =
                scope.spawn_future([]() -> CoroTask<int> { co_return 25; });

            auto [a, b] = co_await when_all(std::move(f1), std::move(f2));

            sum_ptr->store(a + b, std::memory_order_relaxed);
            co_return;
        })));

    CHECK(sum.load() == 42);
}
