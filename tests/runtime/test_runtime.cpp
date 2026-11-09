// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

namespace {

static CoroTask<int> add_async(int a, int b) { co_return a + b; }

static CoroTask<void> noop_async() { co_return; }

static CoroTask<int> throw_async() {
    throw std::runtime_error("test error");
    co_return 0;
}

static CoroTask<dstd::String> string_async() { co_return "hello"; }

static CoroTask<void> throw_void_async() {
    throw std::runtime_error("schedule error");
    co_return;
}

}  // namespace

TEST_CASE("Runtime - an unsized I/O pool has one thread per worker") {
#ifndef DFTRACER_UTILS_VALGRIND_MODE
    if (std::getenv("DFTRACER_UTILS_IO_THREADS") != nullptr) return;
    Runtime rt(3);
    CHECK(rt.io_threads() == 3);
    rt.shutdown();
#endif
}

TEST_CASE("Runtime - parallel_for covers the range exactly once") {
    Runtime rt(4);
    const std::int64_t n = 100000;
    dstd::Vector<std::int32_t> hits(n, 0);
    dftu_utils_test::ok(
        rt.parallel_for(n, 4096, [&](std::int64_t begin, std::int64_t end) {
            for (std::int64_t i = begin; i < end; ++i) hits[i]++;
        }));
    for (std::int64_t i = 0; i < n; ++i) REQUIRE(hits[i] == 1);
}

TEST_CASE("Runtime - parallel_reduce matches a serial sum") {
    Runtime rt(4);
    const std::int64_t n = 200000;
    std::int64_t expected = 0;
    for (std::int64_t i = 0; i < n; ++i) expected += i;
    std::int64_t got = dftu_utils_test::ok(rt.parallel_reduce(
        n, 4096, std::int64_t{0},
        [](std::int64_t b, std::int64_t e) {
            std::int64_t s = 0;
            for (std::int64_t i = b; i < e; ++i) s += i;
            return s;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; }));
    CHECK(got == expected);
}

TEST_CASE("Runtime - parallel_for is serial for a single chunk") {
    Runtime rt(4);
    // grain >= n -> one chunk -> body called once inline, no fork-join.
    int calls = 0;
    dftu_utils_test::ok(
        rt.parallel_for(1000, 100000, [&](std::int64_t b, std::int64_t e) {
            ++calls;
            CHECK(b == 0);
            CHECK(e == 1000);
        }));
    CHECK(calls == 1);
}

TEST_CASE("Runtime - nested parallel_for runs the inner loop serial") {
    Runtime rt(4);
    // The inner parallel_for must see in_parallel_region() and run inline, so
    // every outer chunk's inner call fires exactly once.
    std::atomic<int> inner_calls{0};
    dftu_utils_test::ok(rt.parallel_for(8, 1, [&](std::int64_t, std::int64_t) {
        dftu_utils_test::ok(rt.parallel_for(
            1000, 8,
            [&](std::int64_t, std::int64_t) { inner_calls.fetch_add(1); }));
    }));
    CHECK(inner_calls.load() == 8);  // 8 outer chunks x 1 inner (serial) each
}

TEST_CASE("Runtime - submit returns correct value") {
    Runtime rt(2);
    auto result = dftu_utils_test::ok(rt.submit(add_async(3, 4), "add").get());
    CHECK(result == 7);
}

TEST_CASE("Runtime - submit void completes") {
    Runtime rt(2);
    CHECK_NOTHROW(dftu_utils_test::ok(rt.submit(noop_async(), "noop").wait()));
}

TEST_CASE("Runtime - submit propagates exception") {
    Runtime rt(2);
    CHECK_THROWS_AS(
        dftu_utils_test::ok(rt.submit(throw_async(), "throw").get()),
        std::runtime_error);
}

TEST_CASE("Runtime - submit string result") {
    Runtime rt(2);
    auto result =
        dftu_utils_test::ok(rt.submit(string_async(), "string").get());
    CHECK(result == "hello");
}

TEST_CASE("Runtime - submit runs async coroutine") {
    Runtime rt(2);
    auto h = rt.submit(noop_async(), "async-run");
    dftu_utils_test::ok(h.wait());
    CHECK(h.done());
}

TEST_CASE("Runtime - multiple sequential submits") {
    Runtime rt(2);
    for (int i = 0; i < 10; ++i) {
        auto result =
            dftu_utils_test::ok(rt.submit(add_async(i, i), "add").get());
        CHECK(result == i * 2);
    }
}

TEST_CASE("Runtime - shutdown is idempotent") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "noop").wait());
    rt.shutdown();
    CHECK_NOTHROW(rt.shutdown());
}

TEST_CASE("Runtime - get_progress reports completed task") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "tracked").wait());
    auto progress = rt.progress();
    CHECK(progress.total_tasks_submitted == 1);
    CHECK(progress.tasks_completed == 1);
}

TEST_CASE("Runtime - threads returns configured count") {
    Runtime rt(4);
    CHECK(rt.threads() == 4);
}

TEST_CASE("Runtime - default threads uses hardware_concurrency") {
    Runtime rt;
    CHECK(rt.threads() == common::hardware_concurrency());
}

TEST_CASE("Runtime - is_responsive after submit") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "noop").wait());
    CHECK(rt.is_responsive());
}

TEST_CASE("Runtime - submit after shutdown gives a cancelled result") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "noop").wait());
    rt.shutdown();
    auto refused = rt.submit(noop_async(), "refused").get();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().condition() == dstd::Condition::Cancelled);
}

TEST_CASE("Runtime - submit of a typed task after shutdown is cancelled") {
    Runtime rt(2);
    rt.shutdown();
    auto task = []() -> CoroTask<int> { co_return 5; };
    auto refused = rt.submit(task(), "refused").get();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().condition() == dstd::Condition::Cancelled);
    auto blocked = rt.block_on(task());
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().condition() == dstd::Condition::Cancelled);
}

TEST_CASE("Runtime - a refused handle that nobody reads does not abort") {
    Runtime rt(2);
    rt.shutdown();
    rt.submit(noop_async(), "unread");
}

TEST_CASE("Runtime - submit exception does not crash runtime") {
    Runtime rt(2);
    rt.submit(throw_void_async(), "throw");
    dftu_utils_test::ok(rt.submit(noop_async(), "after_throw").wait());
    CHECK_NOTHROW(dftu_utils_test::ok(rt.submit(noop_async(), "noop").wait()));
}

TEST_CASE("Runtime - progress starts at zero") {
    Runtime rt(2);
    auto p = rt.progress();
    CHECK(p.total_tasks_submitted == 0);
    CHECK(p.tasks_completed == 0);
    CHECK(p.tasks_failed == 0);
    CHECK(p.tasks_running == 0);
    CHECK(p.root_tasks.empty());
    CHECK(p.recent_errors.empty());
}

TEST_CASE("Runtime - progress shows a task as running while it runs") {
    Runtime rt(2);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    auto body = [&]() -> coro::CoroTask<void> {
        started.store(true);
        while (!release.load()) std::this_thread::yield();
        co_return;
    };
    TaskHandle handle = rt.submit(body(), "Busy");
    while (!started.load()) std::this_thread::yield();

    auto running = rt.progress();
    CHECK(running.tasks_running == 1);
    CHECK(running.tasks_queued == 0);
    REQUIRE(running.root_tasks.size() == 1);
    CHECK(running.root_tasks[0].state == TaskState::Running);
    CHECK(running.root_tasks[0].location == TaskLocation::Executing);
    CHECK(running.root_tasks[0].worker >= 0);

    release.store(true);
    dftu_utils_test::ok(handle.wait());
    auto ended = rt.progress();
    CHECK(ended.tasks_running == 0);
    CHECK(ended.tasks_completed == 1);
}

TEST_CASE("Runtime - progress workers present") {
    Runtime rt(2);
    auto p = rt.progress();
    CHECK(p.workers.size() == 2);
    for (const auto &w : p.workers) {
        CHECK(w.local_queue_depth == 0);
    }
}

TEST_CASE("Runtime - progress accumulates across submits") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "a").wait());
    dftu_utils_test::ok(rt.submit(noop_async(), "b").wait());
    dftu_utils_test::ok(rt.submit(add_async(1, 2), "c").wait());
    auto p = rt.progress();
    CHECK(p.total_tasks_submitted == 3);
    CHECK(p.tasks_completed == 3);
    CHECK(p.tasks_failed == 0);
}

TEST_CASE("Runtime - progress tracks async submit") {
    Runtime rt(2);
    rt.submit(noop_async(), "bg");
    rt.submit(noop_async(), "bg2");
    rt.wait_all();
    auto p = rt.progress();
    CHECK(p.total_tasks_submitted >= 2);
    CHECK(p.tasks_completed >= 2);
}

TEST_CASE("Runtime - progress task details") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "my_task").wait());
    auto p = rt.progress();
    REQUIRE(p.root_tasks.size() == 1);
    CHECK(p.root_tasks[0].name == "my_task");
    CHECK(p.root_tasks[0].state == TaskState::Completed);
    CHECK(p.root_tasks[0].execution_duration_ms >= 0.0);
    CHECK(p.root_tasks[0].queued_duration_ms >= 0.0);
    CHECK(p.root_tasks[0].progress_percentage == 100.0);
}

TEST_CASE("Runtime - progress keeps only the task history limit") {
    Runtime rt(ExecutorConfig{.thread_count = 2, .task_history = 2}, false);
    dftu_utils_test::ok(rt.submit(noop_async(), "first").wait());
    dftu_utils_test::ok(rt.submit(noop_async(), "second").wait());
    dftu_utils_test::ok(rt.submit(noop_async(), "third").wait());
    auto p = rt.progress();
    CHECK(p.root_tasks.size() == 2);
    CHECK(p.tasks_completed == 3);
}

TEST_CASE("Runtime - a task that raises counts as failed") {
    Runtime rt(ExecutorConfig{.thread_count = 2, .task_history = 0}, false);
    CHECK_THROWS_AS(
        dftu_utils_test::ok(rt.submit(throw_async(), "throw").get()),
        std::runtime_error);
    CHECK_THROWS_AS(
        dftu_utils_test::ok(rt.submit(throw_void_async(), "throw_void").wait()),
        std::runtime_error);
    auto p = rt.progress();
    CHECK(p.tasks_failed == 2);
    CHECK(p.tasks_completed == 0);
}

TEST_CASE("Runtime - a failed task keeps its message in the history") {
    Runtime rt(ExecutorConfig{.thread_count = 2, .task_history = 2}, false);
    CHECK_THROWS_AS(
        dftu_utils_test::ok(rt.submit(throw_async(), "throw").get()),
        std::runtime_error);
    auto p = rt.progress();
    REQUIRE(p.root_tasks.size() == 1);
    CHECK(p.root_tasks[0].state == TaskState::Failed);
    REQUIRE(p.recent_errors.size() == 1);
    CHECK(p.recent_errors[0].second == "test error");
    CHECK(p.tasks_failed == 1);
}

TEST_CASE("Runtime - progress multiple tasks have names") {
    Runtime rt(2);
    dftu_utils_test::ok(rt.submit(noop_async(), "first").wait());
    dftu_utils_test::ok(rt.submit(add_async(1, 2), "second").wait());
    dftu_utils_test::ok(rt.submit(string_async(), "third").wait());
    auto p = rt.progress();
    REQUIRE(p.root_tasks.size() == 3);
    dstd::Set<dstd::String> names;
    for (const auto &t : p.root_tasks) {
        names.insert(t.name);
        CHECK(t.state == TaskState::Completed);
    }
    CHECK(names.count("first") == 1);
    CHECK(names.count("second") == 1);
    CHECK(names.count("third") == 1);
}

TEST_CASE("Runtime - progress no failures on success") {
    Runtime rt(2);
    for (int i = 0; i < 5; ++i) {
        dftu_utils_test::ok(rt.submit(noop_async(), "ok").wait());
    }
    auto p = rt.progress();
    CHECK(p.tasks_failed == 0);
    CHECK(p.tasks_completed == 5);
    CHECK(p.recent_errors.empty());
}

// ========================================================================
// TaskHandle-specific tests
// ========================================================================

TEST_CASE("TaskHandle - done() returns true after wait") {
    Runtime rt(2);
    auto h = rt.submit(noop_async(), "done-check");
    dftu_utils_test::ok(h.wait());
    CHECK(h.done());
}

TEST_CASE("TaskHandle - name is preserved") {
    Runtime rt(2);
    auto h = rt.submit(noop_async(), "my-custom-name");
    CHECK(h.name == "my-custom-name");
    dftu_utils_test::ok(h.wait());
}

TEST_CASE("TaskHandle - auto-generated name when empty") {
    Runtime rt(2);
    auto h = rt.submit(noop_async());
    CHECK(!h.name.empty());
    CHECK(h.name.substr(0, 5) == "task-");
    dftu_utils_test::ok(h.wait());
}

TEST_CASE("TaskHandle - task_id is assigned") {
    Runtime rt(2);
    auto h = rt.submit(noop_async(), "id-check");
    dftu_utils_test::ok(h.wait());
    // enqueue_tracked assigns ids via fetch_sub from -1000000; sentinel is -1
    CHECK(h.id != -1);
}

TEST_CASE("TypedTaskHandle - get() returns correct value") {
    Runtime rt(2);
    auto h = rt.submit(add_async(10, 20), "typed-get");
    CHECK(dftu_utils_test::ok(h.get()) == 30);
}

TEST_CASE("TypedTaskHandle - get() propagates exception") {
    Runtime rt(2);
    auto h = rt.submit(throw_async(), "typed-throw");
    CHECK_THROWS_AS(dftu_utils_test::ok(h.get()), std::runtime_error);
}

TEST_CASE("TypedTaskHandle - done() and name work") {
    Runtime rt(2);
    auto h = rt.submit(string_async(), "typed-name");
    auto val = dftu_utils_test::ok(h.get());
    CHECK(val == "hello");
    CHECK(h.done());
    CHECK(h.name == "typed-name");
}

TEST_CASE("Runtime - wait_all with no tasks is no-op") {
    Runtime rt(2);
    CHECK_NOTHROW(rt.wait_all());
}

TEST_CASE("Runtime - wait_all waits for multiple tasks") {
    Runtime rt(4);
    for (int i = 0; i < 20; ++i) {
        rt.submit(add_async(i, i), "batch-" + dstd::to_string(i));
    }
    rt.wait_all();
    auto p = rt.progress();
    CHECK(p.tasks_completed == 20);
}

TEST_CASE("Runtime - concurrent submits complete correctly") {
    Runtime rt(4);
    dstd::Vector<TypedTaskHandle<int>> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(
            rt.submit(add_async(i, 100), "concurrent-" + dstd::to_string(i)));
    }
    for (int i = 0; i < 10; ++i) {
        CHECK(dftu_utils_test::ok(handles[i].get()) == i + 100);
    }
}

TEST_CASE("Runtime - wait_all then submit again works") {
    Runtime rt(2);
    rt.submit(noop_async(), "first-batch");
    rt.wait_all();

    rt.submit(noop_async(), "second-batch");
    rt.wait_all();

    auto p = rt.progress();
    CHECK(p.tasks_completed == 2);
}

TEST_CASE("Runtime - auto-generated names are unique") {
    Runtime rt(2);
    auto h1 = rt.submit(noop_async());
    auto h2 = rt.submit(noop_async());
    CHECK(h1.name != h2.name);
    rt.wait_all();
}

TEST_CASE("TaskHandle - void exception via wait") {
    Runtime rt(2);
    auto h = rt.submit(throw_void_async(), "void-throw");
    CHECK_THROWS_AS(dftu_utils_test::ok(h.wait()), std::runtime_error);
    CHECK(h.done());
}

TEST_CASE("TaskHandle - void exception via get") {
    Runtime rt(2);
    auto h = rt.submit(throw_void_async(), "void-get-throw");
    CHECK_THROWS_AS(dftu_utils_test::ok(h.get()), std::runtime_error);
    CHECK(h.done());
}
