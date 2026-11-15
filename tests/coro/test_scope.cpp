// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/sync_wait.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <chrono>

using namespace dftracer::utils;

namespace {

coro::CoroTask<void> bump(std::atomic<int>& count) {
    count.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

coro::CoroTask<void> fail_with_message(const char* message) {
    dstd::Error error{dstd::Condition::Unknown, message};
    auto failure = coro::fail_with(std::move(error));
    co_await failure;
}

coro::CoroTask<void> fail_after(std::atomic<int>& count) {
    count.fetch_add(1, std::memory_order_relaxed);
    co_await fail_with_message("child");
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
        dstd::String label = dstd::fmt<"child-{}">(i);
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
    co_await scope.join_quietly();
    const dstd::Optional<dstd::Error> error = scope.take_error();
    co_return error.has_value() && dstd::String(error->message()) == "child" &&
        scope.stop_requested();
}

coro::CoroTask<void> one_fails_joined(std::atomic<int>& count) {
    coro::Scope scope;
    scope.spawn(bump(count));
    scope.spawn(fail_after(count));
    scope.spawn(bump(count));
    co_await scope.join();
    count.fetch_add(100, std::memory_order_relaxed);
}

coro::CoroTask<int> futures() {
    coro::Scope scope;
    auto a = scope.spawn_future(value(20));
    auto b = scope.spawn_future([]() -> coro::CoroTask<int> { co_return 22; });
    std::atomic<int> count{0};
    auto c = scope.spawn_future(fail_after(count));
    int sum = co_await a;
    sum += co_await b;
    auto settled = co_await coro::settle_in_place(c);
    const bool raised = settled.error.has_value() &&
                        dstd::String(settled.error->message()) == "child";
    co_await scope.join();
    co_return raised ? sum : -1;
}

}  // namespace

TEST_CASE("Scope runs every spawned task before join returns") {
    Runtime rt(4);
    std::atomic<int> count{0};
    dftu_utils_test::ok(rt.submit(spawn_and_join(200, count)).get());
    CHECK(count.load() == 200);
}

TEST_CASE("Scope keeps a spawned lambda's captures alive") {
    Runtime rt(4);
    std::atomic<int> count{0};
    dftu_utils_test::ok(rt.submit(spawn_lambdas(64, count)).get());
    CHECK(count.load() == 64);
}

TEST_CASE("Scope join_quietly gives the first child error after all end") {
    Runtime rt(4);
    std::atomic<int> count{0};
    CHECK(dftu_utils_test::ok(rt.submit(one_fails(count)).get()));
    CHECK(count.load() == 3);
}

TEST_CASE("Scope join ends the awaiting task with the first child error") {
    Runtime rt(4);
    std::atomic<int> count{0};
    const auto result = rt.submit(one_fails_joined(count)).get();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().condition() == dstd::Condition::Unknown);
    CHECK(dstd::String(result.error().message()) == "child");
    CHECK(count.load() == 3);
}

namespace {

struct FrameProbe {
    std::atomic<int>* destroyed;
    ~FrameProbe() { destroyed->fetch_add(1); }
};

}  // namespace

TEST_CASE("an awaited failed child ends the parent and its frame dies once") {
    Runtime rt(2);
    std::atomic<int> destroyed{0};
    std::atomic<int> resumed{0};
    auto parent = [&]() -> coro::CoroTask<int> {
        FrameProbe probe{&destroyed};
        co_await fail_with_message("child failed");
        resumed.fetch_add(1);
        co_return 1;
    };
    {
        const auto result = rt.block_on(parent());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().condition() == dstd::Condition::Unknown);
        CHECK(dstd::String(result.error().message()) == "child failed");
    }
    rt.shutdown();
    CHECK(resumed.load() == 0);
    CHECK(destroyed.load() == 1);
}

TEST_CASE("Scope keeps the first child error when later children fail too") {
    Runtime rt(4);
    auto run = []() -> coro::CoroTask<bool> {
        coro::Scope scope;
        scope.spawn(fail_with_message("first"));
        co_await coro::sleep(std::chrono::milliseconds(50));
        scope.spawn(fail_with_message("second"));
        co_await scope.join_quietly();
        const dstd::Optional<dstd::Error> error = scope.take_error();
        co_return error.has_value() &&
            dstd::String(error->message()) == "first";
    };
    CHECK(dftu_utils_test::ok(rt.block_on(run())));
}

TEST_CASE("spawn_future returns values and keeps errors off the join") {
    Runtime rt(4);
    CHECK(dftu_utils_test::ok(rt.submit(futures()).get()) == 42);
}

TEST_CASE("with_scope joins children when the body fails") {
    Runtime rt(4);
    std::atomic<int> count{0};
    auto body = [&count](coro::Scope& s) -> coro::CoroTask<void> {
        for (int i = 0; i < 50; ++i) s.spawn(bump(count));
        co_await fail_with_message("body");
    };
    const auto result = rt.block_on(coro::with_scope(body));
    REQUIRE_FALSE(result.has_value());
    CHECK(dstd::String(result.error().message()) == "body");
    CHECK(count.load() == 50);
}

TEST_CASE("with_scope returns the body's value") {
    Runtime rt(2);
    int got = dftu_utils_test::ok(
        rt.block_on(coro::with_scope([](coro::Scope& s) -> coro::CoroTask<int> {
            auto f = s.spawn_future(value(5));
            int v = co_await f;
            co_return v + 1;
        })));
    CHECK(got == 6);
}

TEST_CASE("block_on returns the value and the error") {
    Runtime rt(2);
    CHECK(dftu_utils_test::ok(rt.block_on(coro::named("value", value(9)))) ==
          9);
    std::atomic<int> count{0};
    const auto failed = rt.block_on(fail_after(count));
    REQUIRE_FALSE(failed.has_value());
    CHECK(dstd::String(failed.error().message()) == "child");
}

TEST_CASE("block_on from the only worker of a runtime does not deadlock") {
    Runtime rt(1);
    auto outer = [&rt]() -> coro::CoroTask<int> {
        co_return dftu_utils_test::ok(rt.block_on(value(3))) + 1;
    };
    CHECK(dftu_utils_test::ok(rt.block_on(outer())) == 4);
}

TEST_CASE("sync_wait runs the task on the given executor") {
    Runtime rt(2);
    auto where = []() -> coro::CoroTask<coro::Executor*> {
        coro::Executor* ex = co_await coro::this_coro::executor;
        co_return ex;
    };
    CHECK(dftu_utils_test::ok(coro::sync_wait(*rt.executor(), where())) ==
          rt.executor());
    CHECK(dftu_utils_test::ok(coro::sync_wait(*rt.executor(), value(11))) ==
          11);
}

TEST_CASE("a second join returns at once") {
    Runtime rt(2);
    std::atomic<int> count{0};
    auto twice = [&count]() -> coro::CoroTask<bool> {
        coro::Scope scope;
        scope.spawn(bump(count));
        co_await scope.join();
        co_await scope.join();
        co_return true;
    };
    CHECK(dftu_utils_test::ok(rt.block_on(twice())));
    CHECK(count.load() == 1);
}

TEST_CASE("a spawned function that returns a failing task fails the scope") {
    Runtime rt(4);
    std::atomic<int> count{0};
    auto run = [&count]() -> coro::CoroTask<bool> {
        coro::Scope scope;
        scope.spawn(bump(count));
        scope.spawn([]() -> coro::CoroTask<void> {
            co_await fail_with_message("from the function");
        });
        co_await scope.join_quietly();
        const dstd::Optional<dstd::Error> error = scope.take_error();
        co_return error.has_value() &&
            dstd::String(error->message()) == "from the function" &&
            scope.stop_requested();
    };
    CHECK(dftu_utils_test::ok(rt.block_on(run())));
}

TEST_CASE("a spawned future function that returns a failing task fails it") {
    Runtime rt(2);
    auto run = []() -> coro::CoroTask<bool> {
        coro::Scope scope;
        auto f = scope.spawn_future([]() -> coro::CoroTask<int> {
            co_await fail_with_message("from the function");
            co_return 0;
        });
        auto settled = co_await coro::settle_in_place(f);
        const bool raised =
            settled.error.has_value() &&
            dstd::String(settled.error->message()) == "from the function";
        co_await scope.join_quietly();
        co_return raised;
    };
    CHECK(dftu_utils_test::ok(rt.block_on(run())));
}

TEST_CASE("with_scope joins its children when the body ends with an error") {
    Runtime rt(4);
    std::atomic<int> count{0};
    auto body = [&count](coro::Scope& s) -> coro::CoroTask<void> {
        for (int i = 0; i < 50; ++i) s.spawn(bump(count));
        dstd::Error error{dstd::Condition::Unknown, "body"};
        co_await std::move(error);
    };
    const auto result = rt.block_on(coro::with_scope(body));
    REQUIRE_FALSE(result.has_value());
    CHECK(dstd::String(result.error().message()) == "body");
    CHECK(count.load() == 50);
}

namespace {

coro::CoroTask<int> stops_at_once() {
    co_await coro::stop_here();
    co_return 1;
}

}  // namespace

TEST_CASE(
    "a stopped task gives a cancelled result at get, sync_wait and block_on") {
    Runtime rt(2);
    const auto blocked = rt.block_on(stops_at_once());
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().condition() == dstd::Condition::Cancelled);

    const auto submitted = rt.submit(stops_at_once()).get();
    REQUIRE_FALSE(submitted.has_value());
    CHECK(submitted.error().condition() == dstd::Condition::Cancelled);

    const auto waited = coro::sync_wait(*rt.executor(), stops_at_once());
    REQUIRE_FALSE(waited.has_value());
    CHECK(waited.error().condition() == dstd::Condition::Cancelled);

    const auto direct = stops_at_once().get();
    REQUIRE_FALSE(direct.has_value());
    CHECK(direct.error().condition() == dstd::Condition::Cancelled);
}

namespace {

coro::CoroTask<void> parks_for_long() {
    co_await coro::sleep(std::chrono::hours(1));
}

coro::CoroTask<void> awaits_parked_child() { co_await parks_for_long(); }

}  // namespace

// A stop job can end the child on another thread while the parent is still in
// its await_suspend. The window is a few instructions, so this loop makes a
// hit likely but cannot force one; a miss of the ordering ends the process.
TEST_CASE("a stop that lands while a parent suspends on its child is handled") {
    Runtime rt(4);
    for (int round = 0; round < 3000; ++round) {
        dftu_utils_test::ok(rt.block_on(
            coro::with_scope([](coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn(awaits_parked_child());
                scope.spawn(awaits_parked_child());
                scope.request_stop();
                co_return;
            })));
    }
}
