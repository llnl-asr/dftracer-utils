#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/when_any.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <string>

using namespace dftracer::utils;

TEST_CASE("when_any - Two heterogeneous types (int, string)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> index_valid{false};
    std::atomic<bool> value_valid{false};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope([&](CoroScope& scope)
                                        -> coro::CoroTask<void> {
                auto f1 = scope.spawn(
                    [](CoroScope&) -> coro::CoroTask<int> { co_return 42; });
                auto f2 =
                    scope.spawn([](CoroScope&) -> coro::CoroTask<std::string> {
                        co_return std::string("hello");
                    });

                auto result =
                    co_await coro::when_any(std::move(f1), std::move(f2));

                auto* iv = &index_valid;
                auto* vv = &value_valid;

                iv->store(result.index <= 1, std::memory_order_relaxed);

                if (result.index == 0) {
                    vv->store(result.get<0>() == 42, std::memory_order_relaxed);
                } else {
                    vv->store(result.get<1>() == "hello",
                              std::memory_order_relaxed);
                }

                co_return;
            });
            co_return;
        },
        "HeteroTwoTypes");

    scheduler.schedule(task);
    task->wait();
    CHECK(index_valid.load());
    CHECK(value_valid.load());
    executor.shutdown();
}

TEST_CASE("when_any - Three heterogeneous types") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> index_valid{false};
    std::atomic<bool> type_valid{false};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    auto f1 = scope.spawn(
                        [](CoroScope&) -> coro::CoroTask<int> { co_return 1; });
                    auto f2 =
                        scope.spawn([](CoroScope&) -> coro::CoroTask<float> {
                            co_return 2.0f;
                        });
                    auto f3 = scope.spawn(
                        [](CoroScope&) -> coro::CoroTask<std::string> {
                            co_return std::string("three");
                        });

                    auto result = co_await coro::when_any(
                        std::move(f1), std::move(f2), std::move(f3));

                    auto* iv = &index_valid;
                    auto* tv = &type_valid;

                    iv->store(result.index <= 2, std::memory_order_relaxed);

                    bool correct_type = false;
                    switch (result.index) {
                        case 0:
                            correct_type = (result.get<0>() == 1);
                            break;
                        case 1:
                            correct_type = (result.get<1>() == 2.0f);
                            break;
                        case 2:
                            correct_type = (result.get<2>() == "three");
                            break;
                        default:
                            break;
                    }
                    tv->store(correct_type, std::memory_order_relaxed);

                    co_return;
                });
            co_return;
        },
        "HeteroThreeTypes");

    scheduler.schedule(task);
    task->wait();
    CHECK(index_valid.load());
    CHECK(type_valid.load());
    executor.shutdown();
}

TEST_CASE("when_any - Mixed void and non-void") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> index_valid{false};
    std::atomic<bool> type_valid{false};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    auto f1 = scope.spawn(
                        [](CoroScope&) -> coro::CoroTask<void> { co_return; });
                    auto f2 =
                        scope.spawn([](CoroScope&) -> coro::CoroTask<int> {
                            co_return 99;
                        });

                    auto result =
                        co_await coro::when_any(std::move(f1), std::move(f2));

                    auto* iv = &index_valid;
                    auto* tv = &type_valid;

                    iv->store(result.index <= 1, std::memory_order_relaxed);

                    bool correct_type = false;
                    if (result.index == 0) {
                        correct_type = (result.get<0>() == std::monostate{});
                    } else {
                        correct_type = (result.get<1>() == 99);
                    }
                    tv->store(correct_type, std::memory_order_relaxed);

                    co_return;
                });
            co_return;
        },
        "HeteroVoidAndInt");

    scheduler.schedule(task);
    task->wait();
    CHECK(index_valid.load());
    CHECK(type_valid.load());
    executor.shutdown();
}

TEST_CASE("when_any - Exception propagation") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> handled{false};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope([&](CoroScope& scope)
                                        -> coro::CoroTask<void> {
                auto f1 = scope.spawn([](CoroScope&) -> coro::CoroTask<int> {
                    throw std::runtime_error("boom");
                    co_return 0;
                });
                auto f2 =
                    scope.spawn([](CoroScope&) -> coro::CoroTask<std::string> {
                        co_return std::string("ok");
                    });

                auto* h = &handled;
                try {
                    auto result =
                        co_await coro::when_any(std::move(f1), std::move(f2));
                    // The int task may have lost the race; string won.
                    if (result.index == 1) {
                        h->store(result.get<1>() == "ok",
                                 std::memory_order_relaxed);
                    } else {
                        // Should not reach here without exception
                        h->store(false, std::memory_order_relaxed);
                    }
                } catch (const std::runtime_error& e) {
                    // Throwing task won the race
                    h->store(std::string(e.what()) == "boom",
                             std::memory_order_relaxed);
                }

                co_return;
            });
            co_return;
        },
        "HeteroException");

    scheduler.schedule(task);
    task->wait();
    CHECK(handled.load());
    executor.shutdown();
}

TEST_CASE("when_any - Index-based get<N>() access") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> accessed{false};
    std::atomic<bool> value_correct{false};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope([&](CoroScope& scope)
                                        -> coro::CoroTask<void> {
                auto f1 = scope.spawn(
                    [](CoroScope&) -> coro::CoroTask<int> { co_return 42; });
                auto f2 =
                    scope.spawn([](CoroScope&) -> coro::CoroTask<std::string> {
                        co_return std::string("hello");
                    });

                auto result =
                    co_await coro::when_any(std::move(f1), std::move(f2));

                auto* acc = &accessed;
                auto* vc = &value_correct;

                acc->store(true, std::memory_order_relaxed);

                if (result.index == 0) {
                    vc->store(result.get<0>() == 42, std::memory_order_relaxed);
                } else {
                    vc->store(result.get<1>() == "hello",
                              std::memory_order_relaxed);
                }

                co_return;
            });
            co_return;
        },
        "HeteroGetAccess");

    scheduler.schedule(task);
    task->wait();
    CHECK(accessed.load());
    CHECK(value_correct.load());
    executor.shutdown();
}

TEST_CASE("when_any - Overload resolution (homogeneous uses vector version)") {
    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<bool> index_valid{false};
    std::atomic<int> result_value{-1};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.coro_scope(
                [&](CoroScope& scope) -> coro::CoroTask<void> {
                    // Both futures are SpawnFuture<int> -- homogeneous overload
                    // selected; result is WhenAnyResult<int>, not
                    // WhenAnyTupleResult.
                    auto f1 =
                        scope.spawn([](CoroScope&) -> coro::CoroTask<int> {
                            co_return 10;
                        });
                    auto f2 =
                        scope.spawn([](CoroScope&) -> coro::CoroTask<int> {
                            co_return 20;
                        });

                    auto result =
                        co_await coro::when_any(std::move(f1), std::move(f2));

                    // result_type is WhenAnyResult<int>: .result is int
                    static_assert(
                        std::is_same_v<decltype(result.result), int>,
                        "homogeneous when_any must return WhenAnyResult<int>");

                    auto* iv = &index_valid;
                    auto* rv = &result_value;

                    iv->store(result.index <= 1, std::memory_order_relaxed);
                    rv->store(result.result, std::memory_order_relaxed);

                    co_return;
                });
            co_return;
        },
        "HomogeneousOverload");

    scheduler.schedule(task);
    task->wait();
    CHECK(index_valid.load());
    int rv = result_value.load();
    CHECK((rv == 10 || rv == 20));
    executor.shutdown();
}
