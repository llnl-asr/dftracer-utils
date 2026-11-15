// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <coro/failing.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <memory>

using namespace dftracer::utils;

namespace {

// The failed task owns `token` as a parameter of its frame, so the token
// expires exactly when the frame of the failed task is destroyed.
coro::CoroTask<int> fail_int(dstd::SharedPtr<int> token) {
    co_await coro_test::failing_task("task failed");
    co_return *token;
}

coro::CoroTask<void> fail_void(dstd::SharedPtr<int> token) {
    co_await coro_test::failing_task("task failed");
    (void)token;
}

}  // namespace

TEST_CASE("a failed typed task frees its frame once the caller drops it") {
    Runtime rt;
    dstd::WeakPtr<int> watch;
    {
        auto token = dstd::make_shared<int>(1);
        watch = token;
        auto handle = rt.submit(fail_int(std::move(token)));
        const auto result = handle.get();
        REQUIRE_FALSE(result.has_value());
        CHECK(dstd::String(result.error().message()) == "task failed");
    }
    rt.shutdown();
    CHECK(watch.expired());
}

TEST_CASE("a failed untyped task frees its frame at shutdown") {
    Runtime rt;
    dstd::WeakPtr<int> watch;
    {
        auto token = dstd::make_shared<int>(1);
        watch = token;
        auto handle = rt.submit(fail_void(std::move(token)));
        const auto result = handle.get();
        REQUIRE_FALSE(result.has_value());
        CHECK(dstd::String(result.error().message()) == "task failed");
    }
    rt.shutdown();
    CHECK(watch.expired());
}
