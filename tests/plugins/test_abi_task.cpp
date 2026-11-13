// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <doctest/doctest.h>
#include <plugins/test_abi_ctx.h>

#include <atomic>
#include <stdexcept>

using namespace dftracer::utils;
using test_plugin_common::run_task;

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

TEST_SUITE("plugin task ABI") {
    TEST_CASE("the task service run drives a task to completion") {
        std::atomic<int> ran{0};
        dftu_task* t = plugins::task_to_abi(set_flag(&ran));
        CHECK(run_task(t) == DFTU_OK);
        CHECK(ran.load() == 1);
    }

    TEST_CASE("the task service run reports a thrown task with its message") {
        dftu_task* t = plugins::task_to_abi(throw_boom());
        dstd::String message;
        CHECK(run_task(t, &message) < 0);
        CHECK(message.find("boom") != dstd::String::npos);
    }

    TEST_CASE("a task made with make_abi_task fails with its own status") {
        dftu_task* t = plugins::make_abi_task([](plugins::AbiTask* task) {
            return [](plugins::AbiTask* self) -> coro::CoroTask<void> {
                self->fail(DFTU_E_NOT_FOUND, "no such thing");
                co_return;
            }(task);
        });
        dstd::String message;
        CHECK(run_task(t, &message) == DFTU_E_NOT_FOUND);
        CHECK(message == "no such thing");
    }

    TEST_CASE("run_abi_task awaits a task once and repeats its status") {
        std::atomic<int> ran{0};
        dftu_task* t = plugins::task_to_abi(set_flag(&ran));
        plugins::AbiTask* task = plugins::abi_task(t);
        dftu_status first = DFTU_E_UNKNOWN, second = DFTU_E_UNKNOWN;
        auto body = [&]() -> coro::CoroTask<void> {
            first = co_await plugins::run_abi_task(task);
            second = co_await plugins::run_abi_task(task);
        };
        dftu_task* driver = plugins::task_to_abi(body());
        CHECK(run_task(driver) == DFTU_OK);
        CHECK(first == DFTU_OK);
        CHECK(second == DFTU_OK);
        CHECK(ran.load() == 1);
        plugins::free_abi_task(t);
    }
}
