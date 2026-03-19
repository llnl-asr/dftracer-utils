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
    auto result = rt.submit("add", add_async(3, 4));
    CHECK(result == 7);
}

TEST_CASE("Runtime - submit void completes") {
    Runtime rt(2);
    CHECK_NOTHROW(rt.submit("noop", noop_async()));
}

TEST_CASE("Runtime - submit propagates exception") {
    Runtime rt(2);
    CHECK_THROWS_AS(rt.submit("throw", throw_async()), std::runtime_error);
}

TEST_CASE("Runtime - submit string result") {
    Runtime rt(2);
    auto result = rt.submit("string", string_async());
    CHECK(result == "hello");
}

TEST_CASE("Runtime - schedule runs coroutine") {
    Runtime rt(2);
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    rt.schedule("fulfill", fulfill_async(std::move(p)));
    auto status = f.wait_for(std::chrono::seconds(5));
    CHECK(status == std::future_status::ready);
}

TEST_CASE("Runtime - multiple sequential submits") {
    Runtime rt(2);
    for (int i = 0; i < 10; ++i) {
        auto result = rt.submit("add", add_async(i, i));
        CHECK(result == i * 2);
    }
}

TEST_CASE("Runtime - shutdown is idempotent") {
    Runtime rt(2);
    rt.submit("noop", noop_async());
    rt.shutdown();
    CHECK_NOTHROW(rt.shutdown());
}

TEST_CASE("Runtime - get_progress reports completed task") {
    Runtime rt(2);
    rt.submit("tracked", noop_async());
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
    rt.submit("noop", noop_async());
    CHECK(rt.is_responsive());
}

TEST_CASE("Runtime - submit after shutdown throws") {
    Runtime rt(2);
    rt.submit("noop", noop_async());
    rt.shutdown();
    CHECK_THROWS_AS(rt.submit("fail", noop_async()), std::runtime_error);
}

TEST_CASE("Runtime - schedule after shutdown throws") {
    Runtime rt(2);
    rt.shutdown();
    CHECK_THROWS_AS(rt.schedule("fail", noop_async()), std::runtime_error);
}

TEST_CASE("Runtime - schedule exception does not crash") {
    Runtime rt(2);
    rt.schedule("throw", throw_void_async());
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    rt.schedule("after_throw", fulfill_async(std::move(p)));
    f.wait_for(std::chrono::seconds(5));
    CHECK_NOTHROW(rt.submit("noop", noop_async()));
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
    rt.submit("a", noop_async());
    rt.submit("b", noop_async());
    rt.submit("c", add_async(1, 2));
    auto p = rt.get_progress();
    CHECK(p.total_tasks_submitted == 3);
    CHECK(p.tasks_completed == 3);
    CHECK(p.tasks_failed == 0);
}

TEST_CASE("Runtime - progress tracks schedule") {
    Runtime rt(2);
    auto pr = std::make_shared<std::promise<void>>();
    auto f = pr->get_future();
    rt.schedule("bg", fulfill_async(std::move(pr)));
    auto status = f.wait_for(std::chrono::seconds(5));
    REQUIRE(status == std::future_status::ready);
    // submit() acts as a barrier -- blocks until the executor processes it,
    // ensuring the prior schedule()'s mark_coro_completed has run.
    rt.submit("barrier", noop_async());
    auto p = rt.get_progress();
    CHECK(p.total_tasks_submitted >= 2);
    CHECK(p.tasks_completed >= 2);
}

TEST_CASE("Runtime - progress task details") {
    Runtime rt(2);
    rt.submit("my_task", noop_async());
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
    rt.submit("first", noop_async());
    rt.submit("second", add_async(1, 2));
    rt.submit("third", string_async());
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
        rt.submit("ok", noop_async());
    }
    auto p = rt.get_progress();
    CHECK(p.tasks_failed == 0);
    CHECK(p.tasks_completed == 5);
    CHECK(p.recent_errors.empty());
}
