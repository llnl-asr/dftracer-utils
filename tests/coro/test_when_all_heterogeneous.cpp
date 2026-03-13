#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

TEST_CASE("when_all - Two heterogeneous types (int, string)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> int_result{0};
    std::atomic<bool> str_ok{false};

    auto* int_ptr = &int_result;
    auto* str_ptr = &str_ok;

    auto task = make_task(
        [int_ptr, str_ptr](CoroScope& ctx) -> CoroTask<void> {
            co_await ctx.coro_scope([int_ptr, str_ptr](
                                        CoroScope& scope) -> CoroTask<void> {
                auto f1 = scope.spawn(
                    [](CoroScope&) -> CoroTask<int> { co_return 42; });
                auto f2 = scope.spawn([](CoroScope&) -> CoroTask<std::string> {
                    co_return std::string("hello");
                });

                auto [a, b] = co_await when_all(std::move(f1), std::move(f2));

                int_ptr->store(a, std::memory_order_relaxed);
                str_ptr->store(b == "hello", std::memory_order_relaxed);
                co_return;
            });
            co_return;
        },
        "WhenAllTwoHetero");

    scheduler.schedule(task);
    task->wait();

    CHECK(int_result.load() == 42);
    CHECK(str_ok.load() == true);
    executor.shutdown();
}

TEST_CASE("when_all - Three heterogeneous types (int, float, string)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> int_result{0};
    std::atomic<bool> float_ok{false};
    std::atomic<bool> str_ok{false};

    auto* int_ptr = &int_result;
    auto* float_ptr = &float_ok;
    auto* str_ptr = &str_ok;

    auto task = make_task(
        [int_ptr, float_ptr, str_ptr](CoroScope& ctx) -> CoroTask<void> {
            co_await ctx.coro_scope([int_ptr, float_ptr, str_ptr](
                                        CoroScope& scope) -> CoroTask<void> {
                auto f1 = scope.spawn(
                    [](CoroScope&) -> CoroTask<int> { co_return 1; });
                auto f2 = scope.spawn(
                    [](CoroScope&) -> CoroTask<float> { co_return 2.5f; });
                auto f3 = scope.spawn([](CoroScope&) -> CoroTask<std::string> {
                    co_return std::string("three");
                });

                auto [a, b, c] = co_await when_all(std::move(f1), std::move(f2),
                                                   std::move(f3));

                int_ptr->store(a, std::memory_order_relaxed);
                float_ptr->store(b == 2.5f, std::memory_order_relaxed);
                str_ptr->store(c == "three", std::memory_order_relaxed);
                co_return;
            });
            co_return;
        },
        "WhenAllThreeHetero");

    scheduler.schedule(task);
    task->wait();

    CHECK(int_result.load() == 1);
    CHECK(float_ok.load() == true);
    CHECK(str_ok.load() == true);
    executor.shutdown();
}

TEST_CASE("when_all - Mixed void and non-void") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> flag{false};
    std::atomic<int> int_result{0};

    auto* flag_ptr = &flag;
    auto* int_ptr = &int_result;

    auto task = make_task(
        [flag_ptr, int_ptr](CoroScope& ctx) -> CoroTask<void> {
            co_await ctx.coro_scope([flag_ptr, int_ptr](
                                        CoroScope& scope) -> CoroTask<void> {
                auto f1 = scope.spawn([flag_ptr](CoroScope&) -> CoroTask<void> {
                    flag_ptr->store(true, std::memory_order_relaxed);
                    co_return;
                });
                auto f2 = scope.spawn(
                    [](CoroScope&) -> CoroTask<int> { co_return 99; });

                auto [mono, val] =
                    co_await when_all(std::move(f1), std::move(f2));

                // mono is std::monostate -- verify it compiles and holds
                static_assert(std::is_same_v<decltype(mono), std::monostate>);
                int_ptr->store(val, std::memory_order_relaxed);
                co_return;
            });
            co_return;
        },
        "WhenAllMixedVoidNonVoid");

    scheduler.schedule(task);
    task->wait();

    CHECK(flag.load() == true);
    CHECK(int_result.load() == 99);
    executor.shutdown();
}

TEST_CASE("when_all - All void types") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};
    auto* ctr = &counter;

    auto task = make_task(
        [ctr](CoroScope& ctx) -> CoroTask<void> {
            co_await ctx.coro_scope([ctr](CoroScope& scope) -> CoroTask<void> {
                auto f1 = scope.spawn([ctr](CoroScope&) -> CoroTask<void> {
                    ctr->fetch_add(1, std::memory_order_relaxed);
                    co_return;
                });
                auto f2 = scope.spawn([ctr](CoroScope&) -> CoroTask<void> {
                    ctr->fetch_add(1, std::memory_order_relaxed);
                    co_return;
                });

                auto [m1, m2] = co_await when_all(std::move(f1), std::move(f2));

                static_assert(std::is_same_v<decltype(m1), std::monostate>);
                static_assert(std::is_same_v<decltype(m2), std::monostate>);
                co_return;
            });
            co_return;
        },
        "WhenAllAllVoid");

    scheduler.schedule(task);
    task->wait();

    CHECK(counter.load() == 2);
    executor.shutdown();
}

TEST_CASE("when_all - Exception propagation from one task") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> exception_caught{false};
    auto* caught_ptr = &exception_caught;

    auto task = make_task(
        [caught_ptr](CoroScope& ctx) -> CoroTask<void> {
            co_await ctx.coro_scope(
                [caught_ptr](CoroScope& scope) -> CoroTask<void> {
                    auto f1 = scope.spawn([](CoroScope&) -> CoroTask<void> {
                        throw std::runtime_error("boom");
                        co_return;
                    });
                    auto f2 = scope.spawn(
                        [](CoroScope&) -> CoroTask<int> { co_return 7; });

                    try {
                        auto result =
                            co_await when_all(std::move(f1), std::move(f2));
                        (void)result;
                    } catch (const std::runtime_error& e) {
                        caught_ptr->store(std::string(e.what()) == "boom",
                                          std::memory_order_relaxed);
                    }
                    co_return;
                });
            co_return;
        },
        "WhenAllExceptionPropagation");

    scheduler.schedule(task);
    task->wait();

    CHECK(exception_caught.load() == true);
    executor.shutdown();
}

TEST_CASE("when_all - Homogeneous types still work with variadic overload") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};
    auto* sum_ptr = &sum;

    auto task = make_task(
        [sum_ptr](CoroScope& ctx) -> CoroTask<void> {
            co_await ctx.coro_scope([sum_ptr](
                                        CoroScope& scope) -> CoroTask<void> {
                auto f1 = scope.spawn(
                    [](CoroScope&) -> CoroTask<int> { co_return 17; });
                auto f2 = scope.spawn(
                    [](CoroScope&) -> CoroTask<int> { co_return 25; });

                auto [a, b] = co_await when_all(std::move(f1), std::move(f2));

                sum_ptr->store(a + b, std::memory_order_relaxed);
                co_return;
            });
            co_return;
        },
        "WhenAllHomogeneousVariadic");

    scheduler.schedule(task);
    task->wait();

    CHECK(sum.load() == 42);
    executor.shutdown();
}
