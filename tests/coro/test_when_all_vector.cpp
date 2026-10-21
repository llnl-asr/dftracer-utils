#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/coro/when_any.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/pipeline/scheduler.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

namespace {

CoroTask<int> ready_value(int value) { co_return value; }

CoroTask<int> yielded_value(int value) {
    co_await yield();
    co_return value;
}

CoroTask<int> failing_value(int) {
    throw std::runtime_error("child failed");
    co_return 0;
}

CoroTask<bool> ready_flag(bool value) { co_return value; }

CoroTask<bool> yielded_flag(bool value) {
    co_await yield();
    co_return value;
}

CoroTask<int> record_run(std::atomic<int>* ran, int value) {
    ran->fetch_add(1);
    co_return value;
}

CoroTask<void> count_ready(std::atomic<int>* ran) {
    ran->fetch_add(1);
    co_return;
}

CoroTask<void> count_yielded(std::atomic<int>* ran) {
    co_await yield();
    ran->fetch_add(1);
    co_return;
}

// Ends at once as cancelled, as a stopped child does.
struct CancelledAtOnce {
    using result_type = int;

    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    int await_resume() const noexcept { return 0; }
    bool cancelled() const noexcept { return true; }
};

template <typename Body>
void run_on_pool(Body& body) {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);
    auto task =
        make_task([&body](CoroScope&) -> CoroTask<void> { co_await body(); },
                  "WhenAllVector");
    scheduler.schedule(task);
    task->wait();
    executor.shutdown();
}

}  // namespace

TEST_CASE("when_all vector - ready and yielding children keep their order") {
    constexpr int ROUNDS = 200;
    constexpr int CHILDREN = 17;
    std::atomic<int> wrong{0};
    auto body = [&wrong]() -> CoroTask<void> {
        for (int round = 0; round < ROUNDS; ++round) {
            std::vector<CoroTask<int>> children;
            for (int i = 0; i < CHILDREN; ++i) {
                children.push_back(i % 3 == 1 ? yielded_value(i)
                                              : ready_value(i));
            }
            auto values = co_await when_all(std::move(children));
            if (values.size() != CHILDREN) wrong.fetch_add(1);
            for (int i = 0; i < static_cast<int>(values.size()); ++i) {
                if (values[static_cast<std::size_t>(i)] != i)
                    wrong.fetch_add(1);
            }
        }
    };
    run_on_pool(body);
    CHECK(wrong.load() == 0);
}

TEST_CASE("a when_all of bool tasks keeps every result") {
    constexpr int ROUNDS = 300;
    constexpr int CHILDREN = 64;
    std::atomic<int> wrong{0};
    auto body = [&wrong]() -> CoroTask<void> {
        for (int round = 0; round < ROUNDS; ++round) {
            std::vector<CoroTask<bool>> children;
            for (int i = 0; i < CHILDREN; ++i) {
                const bool value = (i + round) % 2 == 0;
                children.push_back(i % 3 == 1 ? yielded_flag(value)
                                              : ready_flag(value));
            }
            auto values = co_await when_all(std::move(children));
            if (values.size() != CHILDREN) wrong.fetch_add(1);
            for (int i = 0; i < static_cast<int>(values.size()); ++i) {
                const bool expected = (i + round) % 2 == 0;
                if (values[static_cast<std::size_t>(i)] != expected) {
                    wrong.fetch_add(1);
                }
            }
        }
    };
    run_on_pool(body);
    CHECK(wrong.load() == 0);
}

TEST_CASE("when_all vector - a failing child does not stop the others") {
    std::atomic<int> ran{0};
    std::atomic<int> caught{0};
    auto body = [&ran, &caught]() -> CoroTask<void> {
        std::vector<CoroTask<int>> valued;
        valued.push_back(ready_value(1));
        valued.push_back(failing_value(2));
        valued.push_back(yielded_value(3));
        valued.push_back(ready_value(4));
        try {
            co_await when_all(std::move(valued));
        } catch (const std::runtime_error&) {
            caught.fetch_add(1);
        }
        std::vector<CoroTask<void>> children;
        children.push_back(count_ready(&ran));
        children.push_back(count_yielded(&ran));
        children.push_back(count_ready(&ran));
        children.push_back(count_yielded(&ran));
        co_await when_all(std::move(children));
        if (ran.load() != 4) caught.fetch_add(100);
    };
    run_on_pool(body);
    CHECK(caught.load() == 1);
    CHECK(ran.load() == 4);
}

TEST_CASE("when_all vector - the awaiter resumes after every child ended") {
    constexpr int ROUNDS = 200;
    constexpr int CHILDREN = 9;
    std::atomic<int> ran{0};
    std::atomic<int> early{0};
    auto body = [&ran, &early]() -> CoroTask<void> {
        for (int round = 0; round < ROUNDS; ++round) {
            const int before = ran.load();
            std::vector<CoroTask<void>> children;
            for (int i = 0; i < CHILDREN; ++i) {
                children.push_back(i % 2 == 0 ? count_ready(&ran)
                                              : count_yielded(&ran));
            }
            co_await when_all(std::move(children));
            if (ran.load() - before != CHILDREN) early.fetch_add(1);
        }
    };
    run_on_pool(body);
    CHECK(early.load() == 0);
    CHECK(ran.load() == ROUNDS * CHILDREN);
}

TEST_CASE("when_any vector - the first child to end wins") {
    std::atomic<int> wrong{0};
    auto body = [&wrong]() -> CoroTask<void> {
        for (int round = 0; round < 100; ++round) {
            std::vector<CoroTask<int>> ready;
            for (int i = 0; i < 5; ++i) ready.push_back(ready_value(i + 10));
            auto first = co_await when_any(std::move(ready));
            if (first.index != 0 || first.result != 10) wrong.fetch_add(1);

            std::vector<CoroTask<int>> mixed;
            mixed.push_back(yielded_value(20));
            mixed.push_back(ready_value(21));
            mixed.push_back(yielded_value(22));
            auto second = co_await when_any(std::move(mixed));
            if (second.result != 20 + static_cast<int>(second.index)) {
                wrong.fetch_add(1);
            }
        }
    };
    run_on_pool(body);
    CHECK(wrong.load() == 0);
}

TEST_CASE("when_any drops a loser that never started") {
    std::atomic<int> ran{0};
    std::atomic<std::size_t> winner{99};
    auto body = [&ran, &winner]() -> CoroTask<void> {
        std::vector<CoroTask<int>> children;
        children.push_back(ready_value(1));
        for (int i = 0; i < 4; ++i) children.push_back(record_run(&ran, 2));
        auto result = co_await when_any(std::move(children));
        winner.store(result.index);
    };
    run_on_pool(body);
    CHECK(winner.load() == 0);
    CHECK(ran.load() == 0);
}

TEST_CASE("when_any delivers the exception of a winner that threw") {
    std::atomic<int> caught{0};
    std::atomic<int> other{0};
    auto body = [&caught, &other]() -> CoroTask<void> {
        std::vector<CoroTask<int>> children;
        children.push_back(failing_value(1));
        children.push_back(ready_value(2));
        try {
            co_await when_any(std::move(children));
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "child failed") caught.fetch_add(1);
        } catch (...) {
            other.fetch_add(1);
        }
    };
    run_on_pool(body);
    CHECK(caught.load() == 1);
    CHECK(other.load() == 0);
}

TEST_CASE("when_any reports a cancelled winner") {
    std::atomic<int> cancelled{0};
    std::atomic<int> other{0};
    auto body = [&cancelled, &other]() -> CoroTask<void> {
        std::vector<CancelledAtOnce> children(2);
        try {
            co_await when_any(std::move(children));
        } catch (const DFTUtilsException& e) {
            if (is_cancelled(e)) {
                cancelled.fetch_add(1);
            } else {
                other.fetch_add(1);
            }
        }
    };
    run_on_pool(body);
    CHECK(cancelled.load() == 1);
    CHECK(other.load() == 0);
}
