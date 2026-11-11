// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/run_on.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

using namespace dftracer::utils;

namespace {

struct Seen {
    coro::Executor* before = nullptr;
    coro::Executor* child_current = nullptr;
    coro::Executor* child_queried = nullptr;
    coro::Executor* after = nullptr;
};

coro::CoroTask<int> child(Seen& seen) {
    seen.child_current = coro::Executor::current();
    seen.child_queried = co_await coro::this_coro::executor;
    co_return 7;
}

coro::CoroTask<int> parent(coro::Executor& other, Seen& seen) {
    seen.before = coro::Executor::current();
    int got = co_await coro::run_on(other, child(seen));
    seen.after = coro::Executor::current();
    co_return got;
}

coro::CoroTask<coro::Executor*> query() {
    coro::Executor* ex = co_await coro::this_coro::executor;
    co_return ex;
}

}  // namespace

TEST_CASE("this_coro::executor names the runtime that runs the task") {
    Runtime rt(2);
    coro::Executor* ex = dftu_utils_test::ok(rt.submit(query()).get());
    CHECK(ex == rt.executor());
}

TEST_CASE(
    "run_on runs the child elsewhere and returns to the caller's executor") {
    Runtime home(2);
    Runtime away(2);
    Seen seen;
    int got =
        dftu_utils_test::ok(home.submit(parent(*away.executor(), seen)).get());
    CHECK(got == 7);
    CHECK(seen.before == home.executor());
    CHECK(seen.child_current == away.executor());
    CHECK(seen.child_queried == away.executor());
    CHECK(seen.after == home.executor());
}
