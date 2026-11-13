// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/abi.h>
#include <dftracer/utils/coro/task_abi.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

coro::CoroTask<void> set_flag(std::atomic<int>* n) {
    n->fetch_add(1);
    co_return;
}

coro::CoroTask<void> throw_boom() {
    throw std::runtime_error("boom");
    co_return;
}

}  // namespace

TEST_SUITE("core task ABI") {
    TEST_CASE("dftu_task_run drives a task to completion") {
        std::atomic<int> ran{0};
        dftu_task* t = coro::task_to_abi(set_flag(&ran));
        CHECK(dftu_task_run(dftu_default_runtime(), t, nullptr) == DFTU_OK);
        CHECK(ran.load() == 1);
    }

    TEST_CASE("dftu_task_run reports a thrown task with its message") {
        dftu_task* t = coro::task_to_abi(throw_boom());
        dftu_error err = DFTU_ERROR_INIT;
        const dftu_status st = dftu_task_run(dftu_default_runtime(), t, &err);
        CHECK(st < 0);
        CHECK(err.status == st);
        REQUIRE(err.message != nullptr);
        CHECK(std::strstr(err.message, "boom") != nullptr);
        dftu_error_release(&err);
    }

    TEST_CASE("a task made with make_abi_task fails with its own status") {
        dftu_task* t = coro::make_abi_task([](coro::AbiTask* task) {
            return [](coro::AbiTask* self) -> coro::CoroTask<void> {
                self->fail(DFTU_E_NOT_FOUND, "no such thing");
                co_return;
            }(task);
        });
        dftu_error err = DFTU_ERROR_INIT;
        CHECK(dftu_task_run(dftu_default_runtime(), t, &err) ==
              DFTU_E_NOT_FOUND);
        CHECK(err.status == DFTU_E_NOT_FOUND);
        REQUIRE(err.message != nullptr);
        CHECK(std::strcmp(err.message, "no such thing") == 0);
        dftu_error_release(&err);
    }

    TEST_CASE("run_abi_task awaits a task once and repeats its status") {
        std::atomic<int> ran{0};
        dftu_task* t = coro::task_to_abi(set_flag(&ran));
        coro::AbiTask* task = coro::abi_task(t);
        dftu_status first = DFTU_E_UNKNOWN, second = DFTU_E_UNKNOWN;
        auto body = [&]() -> coro::CoroTask<void> {
            first = co_await coro::run_abi_task(task);
            second = co_await coro::run_abi_task(task);
        };
        dftu_task* driver = coro::task_to_abi(body());
        CHECK(dftu_task_run(dftu_default_runtime(), driver, nullptr) ==
              DFTU_OK);
        CHECK(first == DFTU_OK);
        CHECK(second == DFTU_OK);
        CHECK(ran.load() == 1);
        coro::free_abi_task(t);
    }
}
