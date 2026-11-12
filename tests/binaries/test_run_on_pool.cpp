// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/cli/common_cli.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#include <chrono>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

cli::RuntimeArgs small_runtime() {
    cli::RuntimeArgs runtime;
    runtime.executor_threads = 2;
    runtime.io_threads = 1;
    return runtime;
}

}  // namespace

TEST_CASE("run_on_pool returns the value of the body") {
    auto result = cli::run_on_pool(
        "value", small_runtime(),
        [](coro::Scope&) -> coro::CoroTask<int> { co_return 7; });
    REQUIRE(result.has_value());
    CHECK(*result == 7);
}

TEST_CASE("run_on_pool returns the error result of the body") {
    auto result = cli::run_on_pool(
        "fail result", small_runtime(),
        [](coro::Scope&) -> coro::CoroTask<dstd::Result<int>> {
            co_return dstd::Unexpected<dstd::Error>(
                dstd::Error{dstd::Condition::InvalidArgument, "bad input"});
        });
    REQUIRE(!result.has_value());
    CHECK(result.error().is(dstd::Condition::InvalidArgument));
    CHECK(dstd::StringView(result.error().c_str()) == "bad input");
}

TEST_CASE("run_on_pool raises the exception of the body") {
    CHECK_THROWS_AS(cli::run_on_pool("fail", small_runtime(),
                                     [](coro::Scope&) -> coro::CoroTask<void> {
                                         throw std::runtime_error("boom");
                                         co_return;
                                     })
                        .discard(),
                    std::runtime_error);
}

TEST_CASE("run_on_pool with a watchdog returns the value of the body") {
    cli::WatchdogArgs watchdog;
    auto result = cli::run_on_pool(
        "watched", small_runtime(), watchdog,
        [](coro::Scope&) -> coro::CoroTask<int> { co_return 3; });
    REQUIRE(result.has_value());
    CHECK(*result == 3);
}

TEST_CASE("a watchdog timeout stops the task and raises a timeout error") {
    cli::WatchdogArgs watchdog;
    watchdog.global_timeout = 1;
    watchdog.interval = 1;
    const auto start = std::chrono::steady_clock::now();
    auto result =
        cli::run_on_pool("slow", small_runtime(), watchdog,
                         [](coro::Scope&) -> coro::CoroTask<void> {
                             co_await coro::sleep(std::chrono::seconds(60));
                         });
    REQUIRE(!result.has_value());
    CHECK(result.error().is(dstd::Condition::Timeout));
    CHECK(dstd::StringView(result.error().c_str()).find("Global timeout") !=
          dstd::StringView::npos);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
}

TEST_CASE("a watchdog task timeout names the task and stops it") {
    cli::WatchdogArgs watchdog;
    watchdog.task_timeout = 1;
    watchdog.interval = 1;
    const auto start = std::chrono::steady_clock::now();
    auto result =
        cli::run_on_pool("slow_task", small_runtime(), watchdog,
                         [](coro::Scope&) -> coro::CoroTask<void> {
                             co_await coro::sleep(std::chrono::seconds(60));
                         });
    REQUIRE(!result.has_value());
    CHECK(result.error().is(dstd::Condition::Timeout));
    CHECK(dstd::StringView(result.error().c_str())
              .find("Task 'slow_task' timed out") != dstd::StringView::npos);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
}

TEST_CASE("a watchdog timeout raises for a body that gives a value") {
    cli::WatchdogArgs watchdog;
    watchdog.global_timeout = 1;
    watchdog.interval = 1;
    auto result =
        cli::run_on_pool("slow value", small_runtime(), watchdog,
                         [](coro::Scope&) -> coro::CoroTask<int> {
                             co_await coro::sleep(std::chrono::seconds(60));
                             co_return 1;
                         });
    REQUIRE(!result.has_value());
    CHECK(result.error().is(dstd::Condition::Timeout));
}

TEST_CASE("a disabled watchdog does not stop a task") {
    cli::WatchdogArgs watchdog;
    watchdog.disable = true;
    watchdog.global_timeout = 1;
    auto result = cli::run_on_pool(
        "unwatched", small_runtime(), watchdog,
        [](coro::Scope&) -> coro::CoroTask<int> {
            co_await coro::sleep(std::chrono::milliseconds(1500));
            co_return 5;
        });
    REQUIRE(result.has_value());
    CHECK(*result == 5);
}
