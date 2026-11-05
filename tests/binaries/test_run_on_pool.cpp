// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#include <chrono>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

binaries::RuntimeArgs small_runtime() {
    binaries::RuntimeArgs runtime;
    runtime.executor_threads = 2;
    runtime.io_threads = 1;
    return runtime;
}

}  // namespace

TEST_CASE("run_on_pool returns the value of the body") {
    const int value = binaries::run_on_pool(
        "value", small_runtime(),
        [](coro::Scope&) -> coro::CoroTask<int> { co_return 7; });
    CHECK(value == 7);
}

TEST_CASE("run_on_pool raises the error of the body") {
    CHECK_THROWS_AS(
        binaries::run_on_pool("fail", small_runtime(),
                              [](coro::Scope&) -> coro::CoroTask<void> {
                                  throw std::runtime_error("boom");
                                  co_return;
                              }),
        std::runtime_error);
}

TEST_CASE("run_on_pool with a watchdog returns the value of the body") {
    binaries::WatchdogArgs watchdog;
    const int value = binaries::run_on_pool(
        "watched", small_runtime(), watchdog,
        [](coro::Scope&) -> coro::CoroTask<int> { co_return 3; });
    CHECK(value == 3);
}

TEST_CASE("a watchdog timeout stops the task and raises a timeout error") {
    binaries::WatchdogArgs watchdog;
    watchdog.global_timeout = 1;
    watchdog.interval = 1;
    const auto start = std::chrono::steady_clock::now();
    try {
        binaries::run_on_pool(
            "slow", small_runtime(), watchdog,
            [](coro::Scope&) -> coro::CoroTask<void> {
                co_await coro::sleep(std::chrono::seconds(60));
            });
        FAIL("the timeout did not raise");
    } catch (const common::DFTUtilsException& e) {
        CHECK(e.condition() == dstd::Condition::Timeout);
        CHECK(dstd::StringView(e.what()).find("Global timeout") !=
              dstd::StringView::npos);
    }
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
}

TEST_CASE("a watchdog timeout raises for a body that gives a value") {
    binaries::WatchdogArgs watchdog;
    watchdog.global_timeout = 1;
    watchdog.interval = 1;
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        binaries::run_on_pool(
            "slow value", small_runtime(), watchdog,
            [](coro::Scope&) -> coro::CoroTask<int> {
                co_await coro::sleep(std::chrono::seconds(60));
                co_return 1;
            });
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Timeout);
}

TEST_CASE("a disabled watchdog does not stop a task") {
    binaries::WatchdogArgs watchdog;
    watchdog.disable = true;
    watchdog.global_timeout = 1;
    const int value = binaries::run_on_pool(
        "unwatched", small_runtime(), watchdog,
        [](coro::Scope&) -> coro::CoroTask<int> {
            co_await coro::sleep(std::chrono::milliseconds(1500));
            co_return 5;
        });
    CHECK(value == 5);
}
