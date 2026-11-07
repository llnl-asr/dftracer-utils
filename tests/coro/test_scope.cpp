// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/sync_wait.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>

using namespace dftracer::utils;

namespace {

coro::CoroTask<void> bump(std::atomic<int>& count) {
    count.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

coro::CoroTask<void> fail_after(std::atomic<int>& count) {
    count.fetch_add(1, std::memory_order_relaxed);
    throw common::DFTUtilsException(common::ErrorCode::Pipeline, "child");
    co_return;
}

coro::CoroTask<int> value(int v) { co_return v; }

coro::CoroTask<void> spawn_and_join(int n, std::atomic<int>& count) {
    coro::Scope scope;
    for (int i = 0; i < n; ++i) scope.spawn(bump(count));
    co_await scope.join();
}

coro::CoroTask<void> spawn_lambdas(int n, std::atomic<int>& count) {
    coro::Scope scope;
    for (int i = 0; i < n; ++i) {
        dstd::String label = "child-" + dstd::to_string(i);
        scope.spawn([label, &count]() -> coro::CoroTask<void> {
            if (label.rfind("child-", 0) == 0) count.fetch_add(1);
            co_return;
        });
    }
    co_await scope.join();
}

coro::CoroTask<bool> one_fails(std::atomic<int>& count) {
    coro::Scope scope;
    scope.spawn(bump(count));
    scope.spawn(fail_after(count));
    scope.spawn(bump(count));
    bool raised = false;
    try {
        co_await scope.join();
    } catch (const common::DFTUtilsException&) {
        raised = true;
    }
    co_return raised&& scope.stop_requested();
}

coro::CoroTask<int> futures() {
    coro::Scope scope;
    auto a = scope.spawn_future(value(20));
    auto b = scope.spawn_future([]() -> coro::CoroTask<int> { co_return 22; });
    std::atomic<int> count{0};
    auto c = scope.spawn_future(fail_after(count));
    int sum = co_await a;
    sum += co_await b;
    bool raised = false;
    try {
        co_await c;
    } catch (const common::DFTUtilsException&) {
        raised = true;
    }
    co_await scope.join();
    co_return raised ? sum : -1;
}

coro::CoroTask<void> self_join(coro::Scope& scope, bool& raised) {
    try {
        co_await scope.join();
    } catch (const common::DFTUtilsException&) {
        raised = true;
    }
}

coro::CoroTask<bool> detects_self_join() {
    coro::Scope scope;
    bool raised = false;
    scope.spawn(self_join(scope, raised));
    co_await scope.join();
    co_return raised;
}

coro::CoroTask<bool> refuses_after_close(std::atomic<int>& count) {
    coro::Scope scope;
    scope.close();
    scope.spawn(bump(count));
    bool raised = false;
    try {
        co_await scope.join();
    } catch (const common::DFTUtilsException&) {
        raised = true;
    }
    co_return raised;
}

}  // namespace

TEST_CASE("Scope runs every spawned task before join returns") {
    Runtime rt(4);
    std::atomic<int> count{0};
    rt.submit(spawn_and_join(200, count)).get();
    CHECK(count.load() == 200);
}

TEST_CASE("Scope keeps a spawned lambda's captures alive") {
    Runtime rt(4);
    std::atomic<int> count{0};
    rt.submit(spawn_lambdas(64, count)).get();
    CHECK(count.load() == 64);
}

TEST_CASE("Scope join raises the first child error after all children end") {
    Runtime rt(4);
    std::atomic<int> count{0};
    CHECK(rt.submit(one_fails(count)).get());
    CHECK(count.load() == 3);
}

TEST_CASE("spawn_future returns values and keeps errors off the join") {
    Runtime rt(4);
    CHECK(rt.submit(futures()).get() == 42);
}

TEST_CASE("a task cannot join the scope that spawned it") {
    Runtime rt(2);
    CHECK(rt.submit(detects_self_join()).get());
}

TEST_CASE("a closed scope refuses spawns and join reports it") {
    Runtime rt(2);
    std::atomic<int> count{0};
    CHECK(rt.submit(refuses_after_close(count)).get());
    CHECK(count.load() == 0);
}

TEST_CASE("with_scope joins children when the body raises") {
    Runtime rt(4);
    std::atomic<int> count{0};
    auto body = [&count](coro::Scope& s) -> coro::CoroTask<void> {
        for (int i = 0; i < 50; ++i) s.spawn(bump(count));
        throw common::DFTUtilsException(common::ErrorCode::Pipeline, "body");
        co_return;
    };
    CHECK_THROWS_AS(rt.block_on(coro::with_scope(body)),
                    common::DFTUtilsException);
    CHECK(count.load() == 50);
}

TEST_CASE("with_scope returns the body's value") {
    Runtime rt(2);
    int got =
        rt.block_on(coro::with_scope([](coro::Scope& s) -> coro::CoroTask<int> {
            auto f = s.spawn_future(value(5));
            int v = co_await f;
            co_return v + 1;
        }));
    CHECK(got == 6);
}

TEST_CASE("block_on returns the value and raises the error") {
    Runtime rt(2);
    CHECK(rt.block_on(coro::named("value", value(9))) == 9);
    std::atomic<int> count{0};
    CHECK_THROWS_AS(rt.block_on(fail_after(count)), common::DFTUtilsException);
}

TEST_CASE("block_on from the only worker of a runtime does not deadlock") {
    Runtime rt(1);
    auto outer = [&rt]() -> coro::CoroTask<int> {
        co_return rt.block_on(value(3)) + 1;
    };
    CHECK(rt.block_on(outer()) == 4);
}

TEST_CASE("sync_wait runs the task on the given executor") {
    Runtime rt(2);
    auto where = []() -> coro::CoroTask<coro::Executor*> {
        coro::Executor* ex = co_await coro::this_coro::executor;
        co_return ex;
    };
    CHECK(coro::sync_wait(*rt.executor(), where()) == rt.executor());
    CHECK(coro::sync_wait(*rt.executor(), value(11)) == 11);
}

TEST_CASE("a second join returns at once and the joined scope is closed") {
    Runtime rt(2);
    std::atomic<int> count{0};
    auto twice = [&count]() -> coro::CoroTask<bool> {
        coro::Scope scope;
        scope.spawn(bump(count));
        co_await scope.join();
        co_await scope.join();
        scope.spawn(bump(count));
        bool raised = false;
        try {
            co_await scope.join();
        } catch (const common::DFTUtilsException&) {
            raised = true;
        }
        co_return raised;
    };
    CHECK(rt.block_on(twice()));
    CHECK(count.load() == 1);
}
