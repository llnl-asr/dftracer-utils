#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

namespace {

static CoroTask<int> add_async(int a, int b) { co_return a + b; }

static CoroTask<void> noop_async() { co_return; }

static CoroTask<int> throw_async() {
    throw std::runtime_error("test error");
    co_return 0;
}

static CoroTask<void> fulfill_async(std::shared_ptr<std::promise<void>> p) {
    p->set_value();
    co_return;
}

static CoroTask<std::string> string_async() { co_return "hello"; }

static CoroTask<void> throw_void_async() {
    throw std::runtime_error("schedule error");
    co_return;
}

}  // namespace

TEST_CASE("Runtime - submit returns correct value") {
    Runtime rt(2);
    auto result = rt.submit(add_async(3, 4), "add").get();
    CHECK(result == 7);
}

TEST_CASE("Runtime - submit void completes") {
    Runtime rt(2);
    CHECK_NOTHROW(rt.submit(noop_async(), "noop").wait());
}

TEST_CASE("Runtime - submit propagates exception") {
    Runtime rt(2);
    CHECK_THROWS_AS(rt.submit(throw_async(), "throw").get(),
                    std::runtime_error);
}

TEST_CASE("Runtime - submit string result") {
    Runtime rt(2);
    auto result = rt.submit(string_async(), "string").get();
    CHECK(result == "hello");
}

TEST_CASE("Runtime - submit runs async coroutine") {
    Runtime rt(2);
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    rt.submit(fulfill_async(std::move(p)), "fulfill");
    auto status = f.wait_for(std::chrono::seconds(5));
    CHECK(status == std::future_status::ready);
}

TEST_CASE("Runtime - multiple sequential submits") {
    Runtime rt(2);
    for (int i = 0; i < 10; ++i) {
        auto result = rt.submit(add_async(i, i), "add").get();
        CHECK(result == i * 2);
    }
}

TEST_CASE("Runtime - shutdown is idempotent") {
    Runtime rt(2);
    rt.submit(noop_async(), "noop").wait();
    rt.shutdown();
    CHECK_NOTHROW(rt.shutdown());
}

TEST_CASE("Runtime - get_progress reports completed task") {
    Runtime rt(2);
    rt.submit(noop_async(), "tracked").wait();
    auto progress = rt.get_progress();
    CHECK(progress.total_tasks_submitted == 1);
    CHECK(progress.tasks_completed == 1);
}

TEST_CASE("Runtime - threads returns configured count") {
    Runtime rt(4);
    CHECK(rt.threads() == 4);
}

TEST_CASE("Runtime - default threads uses hardware_concurrency") {
    Runtime rt;
    CHECK(rt.threads() == std::thread::hardware_concurrency());
}

TEST_CASE("Runtime - is_responsive after submit") {
    Runtime rt(2);
    rt.submit(noop_async(), "noop").wait();
    CHECK(rt.is_responsive());
}

TEST_CASE("Runtime - submit after shutdown throws") {
    Runtime rt(2);
    rt.submit(noop_async(), "noop").wait();
    rt.shutdown();
    CHECK_THROWS_AS(rt.submit(noop_async(), "fail"), std::runtime_error);
}

TEST_CASE("Runtime - submit after shutdown throws (void)") {
    Runtime rt(2);
    rt.shutdown();
    CHECK_THROWS_AS(rt.submit(noop_async(), "fail"), std::runtime_error);
}

TEST_CASE("Runtime - submit exception does not crash runtime") {
    Runtime rt(2);
    rt.submit(throw_void_async(), "throw");
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    rt.submit(fulfill_async(std::move(p)), "after_throw");
    f.wait_for(std::chrono::seconds(5));
    CHECK_NOTHROW(rt.submit(noop_async(), "noop").wait());
}

TEST_CASE("Runtime - progress starts at zero") {
    Runtime rt(2);
    auto p = rt.get_progress();
    CHECK(p.total_tasks_submitted == 0);
    CHECK(p.tasks_completed == 0);
    CHECK(p.tasks_failed == 0);
    CHECK(p.tasks_running == 0);
    CHECK(p.root_tasks.empty());
    CHECK(p.recent_errors.empty());
}

TEST_CASE("Runtime - progress workers present") {
    Runtime rt(2);
    auto p = rt.get_progress();
    CHECK(p.workers.size() == 2);
    for (const auto &w : p.workers) {
        CHECK(w.local_queue_depth == 0);
    }
}

TEST_CASE("Runtime - progress accumulates across submits") {
    Runtime rt(2);
    rt.submit(noop_async(), "a").wait();
    rt.submit(noop_async(), "b").wait();
    rt.submit(add_async(1, 2), "c").wait();
    auto p = rt.get_progress();
    CHECK(p.total_tasks_submitted == 3);
    CHECK(p.tasks_completed == 3);
    CHECK(p.tasks_failed == 0);
}

TEST_CASE("Runtime - progress tracks async submit") {
    Runtime rt(2);
    auto pr = std::make_shared<std::promise<void>>();
    auto f = pr->get_future();
    rt.submit(fulfill_async(std::move(pr)), "bg");
    auto status = f.wait_for(std::chrono::seconds(5));
    REQUIRE(status == std::future_status::ready);
    // wait() acts as a barrier -- ensures the prior submit()'s
    // mark_coro_completed has run before we snapshot progress.
    rt.submit(noop_async(), "barrier").wait();
    auto p = rt.get_progress();
    CHECK(p.total_tasks_submitted >= 2);
    CHECK(p.tasks_completed >= 2);
}

TEST_CASE("Runtime - progress task details") {
    Runtime rt(2);
    rt.submit(noop_async(), "my_task").wait();
    auto p = rt.get_progress();
    REQUIRE(p.root_tasks.size() == 1);
    CHECK(p.root_tasks[0].name == "my_task");
    CHECK(p.root_tasks[0].state == "completed");
    CHECK(p.root_tasks[0].execution_duration_ms >= 0.0);
    CHECK(p.root_tasks[0].queued_duration_ms >= 0.0);
    CHECK(p.root_tasks[0].progress_percentage == 100.0);
}

TEST_CASE("Runtime - progress multiple tasks have names") {
    Runtime rt(2);
    rt.submit(noop_async(), "first").wait();
    rt.submit(add_async(1, 2), "second").wait();
    rt.submit(string_async(), "third").wait();
    auto p = rt.get_progress();
    REQUIRE(p.root_tasks.size() == 3);
    std::set<std::string> names;
    for (const auto &t : p.root_tasks) {
        names.insert(t.name);
        CHECK(t.state == "completed");
    }
    CHECK(names.count("first") == 1);
    CHECK(names.count("second") == 1);
    CHECK(names.count("third") == 1);
}

TEST_CASE("Runtime - progress no failures on success") {
    Runtime rt(2);
    for (int i = 0; i < 5; ++i) {
        rt.submit(noop_async(), "ok").wait();
    }
    auto p = rt.get_progress();
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
    h.wait();
    CHECK(h.done());
}

TEST_CASE("TaskHandle - name is preserved") {
    Runtime rt(2);
    auto h = rt.submit(noop_async(), "my-custom-name");
    CHECK(h.name == "my-custom-name");
    h.wait();
}

TEST_CASE("TaskHandle - auto-generated name when empty") {
    Runtime rt(2);
    auto h = rt.submit(noop_async());
    CHECK(!h.name.empty());
    CHECK(h.name.substr(0, 5) == "task-");
    h.wait();
}

TEST_CASE("TaskHandle - task_id is assigned") {
    Runtime rt(2);
    auto h = rt.submit(noop_async(), "id-check");
    h.wait();
    // enqueue_tracked assigns ids via fetch_sub from -1000000; sentinel is -1
    CHECK(h.id != -1);
}

TEST_CASE("TypedTaskHandle - get() returns correct value") {
    Runtime rt(2);
    auto h = rt.submit(add_async(10, 20), "typed-get");
    CHECK(h.get() == 30);
}

TEST_CASE("TypedTaskHandle - get() propagates exception") {
    Runtime rt(2);
    auto h = rt.submit(throw_async(), "typed-throw");
    CHECK_THROWS_AS(h.get(), std::runtime_error);
}

TEST_CASE("TypedTaskHandle - done() and name work") {
    Runtime rt(2);
    auto h = rt.submit(string_async(), "typed-name");
    auto val = h.get();
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
        rt.submit(add_async(i, i), "batch-" + std::to_string(i));
    }
    rt.wait_all();
    auto p = rt.get_progress();
    CHECK(p.tasks_completed == 20);
}

TEST_CASE("Runtime - concurrent submits complete correctly") {
    Runtime rt(4);
    std::vector<TypedTaskHandle<int>> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(
            rt.submit(add_async(i, 100), "concurrent-" + std::to_string(i)));
    }
    for (int i = 0; i < 10; ++i) {
        CHECK(handles[i].get() == i + 100);
    }
}

TEST_CASE("Runtime - wait_all then submit again works") {
    Runtime rt(2);
    rt.submit(noop_async(), "first-batch");
    rt.wait_all();

    rt.submit(noop_async(), "second-batch");
    rt.wait_all();

    auto p = rt.get_progress();
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
    CHECK_THROWS_AS(h.wait(), std::runtime_error);
    CHECK(h.done());
}

TEST_CASE("TaskHandle - void exception via get") {
    Runtime rt(2);
    auto h = rt.submit(throw_void_async(), "void-get-throw");
    CHECK_THROWS_AS(h.get(), std::runtime_error);
    CHECK(h.done());
}
