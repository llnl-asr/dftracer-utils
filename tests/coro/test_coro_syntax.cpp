#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

// ============================================================================
// CoroTask Combinator Tests
// ============================================================================

TEST_CASE("CoroTask - then() combinator") {
    auto task = make_task([]() -> CoroTask<int> { co_return 42; }, "Source");

    auto config =
        PipelineConfig().with_name("ThenTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 42);
}

TEST_CASE("CoroTask - then() with transformation inside coroutine") {
    auto parent_task = make_task(
        []() -> CoroTask<std::string> {
            // Create a coroutine that returns int
            auto compute = []() -> CoroTask<int> { co_return 21; };

            // Use then() to transform the result
            auto transformed = compute().then([](int x) { return x * 2; });

            int result = co_await std::move(transformed);
            co_return std::to_string(result);
        },
        "Parent");

    auto config =
        PipelineConfig().with_name("ThenTransform").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    std::string result = parent_task->get<std::string>();
    CHECK(result == "42");
}

TEST_CASE("CoroTask - operator> for chaining") {
    auto parent_task = make_task(
        []() -> CoroTask<int> {
            auto compute = []() -> CoroTask<int> { co_return 10; };

            // Chain multiple transformations using >>
            auto result = co_await (
                compute() > [](int x) { return x + 5; } >
                [](int x) { return x * 2; });

            co_return result;
        },
        "ChainTest");

    auto config = PipelineConfig().with_name("OpChain").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    int result = parent_task->get<int>();
    CHECK(result == 30);  // (10 + 5) * 2
}

TEST_CASE("CoroTask - tap() for inspection without transformation") {
    std::atomic<int> inspected_value{0};

    auto parent_task = make_task(
        [&inspected_value]() -> CoroTask<int> {
            auto compute = []() -> CoroTask<int> { co_return 99; };

            // Use tap() to inspect without changing the value
            auto result = co_await (compute().tap(
                [&inspected_value](int x) { inspected_value = x; }));

            co_return result;
        },
        "TapTest");

    auto config =
        PipelineConfig().with_name("TapInspect").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    int result = parent_task->get<int>();
    CHECK(result == 99);
    CHECK(inspected_value.load() == 99);
}

TEST_CASE("CoroTask - tap() chained with then()") {
    std::vector<int> log;

    auto parent_task = make_task(
        [&log]() -> CoroTask<std::string> {
            auto compute = []() -> CoroTask<int> { co_return 5; };

            // Chain tap and then
            auto result =
                co_await (compute()
                              .tap([&log](int x) { log.push_back(x); })
                              .then([](int x) { return x * 10; })
                              .tap([&log](int x) { log.push_back(x); })
                              .then([](int x) { return std::to_string(x); }));

            co_return result;
        },
        "TapThenChain");

    auto config =
        PipelineConfig().with_name("TapThenTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    std::string result = parent_task->get<std::string>();
    CHECK(result == "50");
    CHECK(log.size() == 2);
    CHECK(log[0] == 5);
    CHECK(log[1] == 50);
}

TEST_CASE("CoroTask<void> - then() combinator") {
    std::atomic<bool> work_done{false};

    auto parent_task = make_task(
        [&work_done]() -> CoroTask<int> {
            auto do_work = [&work_done]() -> CoroTask<void> {
                work_done = true;
                co_return;
            };

            // Chain void task with value-returning function
            auto result = co_await (do_work().then([]() { return 42; }));

            co_return result;
        },
        "VoidThen");

    auto config =
        PipelineConfig().with_name("VoidThenTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    int result = parent_task->get<int>();
    CHECK(result == 42);
    CHECK(work_done.load() == true);
}

TEST_CASE("CoroTask<void> - tap() for side effects") {
    std::atomic<int> side_effect_count{0};

    auto parent_task = make_task(
        [&side_effect_count]() -> CoroTask<int> {
            auto do_work = []() -> CoroTask<void> { co_return; };

            // Use tap on void task
            co_await (
                do_work()
                    .tap([&side_effect_count]() { side_effect_count++; })
                    .tap([&side_effect_count]() { side_effect_count++; }));

            co_return side_effect_count.load();
        },
        "VoidTap");

    auto config =
        PipelineConfig().with_name("VoidTapTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    int result = parent_task->get<int>();
    CHECK(result == 2);
    CHECK(side_effect_count.load() == 2);
}

TEST_CASE("CoroTask - operator< reverse composition") {
    auto parent_task = make_task(
        []() -> CoroTask<std::string> {
            auto compute = []() -> CoroTask<int> { co_return 7; };

            // Reverse composition: func < task
            auto result = co_await (
                [](int x) { return std::to_string(x * 3); } < compute());

            co_return result;
        },
        "ReverseComp");

    auto config =
        PipelineConfig().with_name("ReverseTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(parent_task);
    auto output = pipeline.execute();

    std::string result = parent_task->get<std::string>();
    CHECK(result == "21");
}

// ============================================================================
// Task Combinator Tests
// ============================================================================

TEST_CASE("Task - then() creates dependent task") {
    std::vector<int> execution_order;

    auto task1 = make_task(
        [&execution_order](CoroScope&) -> CoroTask<int> {
            execution_order.push_back(1);
            co_return 10;
        },
        "Task1");

    auto task2 = task1->then(
        [&execution_order](CoroScope&, int x) -> CoroTask<int> {
            execution_order.push_back(2);
            co_return x * 2;
        },
        "Task2");

    auto config =
        PipelineConfig().with_name("TaskThen").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(task2);

    auto output = pipeline.execute();

    int result = task2->get<int>();
    CHECK(result == 20);
    CHECK(execution_order == std::vector<int>{1, 2});
}

TEST_CASE("Task - operator> for chaining") {
    std::atomic<int> sum{0};

    auto task1 = make_task(
        [&sum](CoroScope&) -> CoroTask<int> {
            sum += 1;
            co_return 5;
        },
        "Task1");

    // Chain using > operator
    auto task2 = task1 > [&sum](CoroScope&, int x) -> CoroTask<int> {
        sum += 10;
        co_return x + 10;
    };

    auto task3 = task2 > [&sum](CoroScope&, int x) -> CoroTask<int> {
        sum += 100;
        co_return x * 2;
    };

    auto config =
        PipelineConfig().with_name("TaskOpChain").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(task3);

    auto output = pipeline.execute();

    int result = task3->get<int>();
    CHECK(result == 30);       // (5 + 10) * 2
    CHECK(sum.load() == 111);  // 1 + 10 + 100
}

TEST_CASE("Task - operator<< reverse composition") {
    auto task1 =
        make_task([](CoroScope&) -> CoroTask<int> { co_return 8; }, "Task1");

    // Reverse composition: func < task
    auto task2 = [](CoroScope&, int x) -> CoroTask<std::string> {
        co_return std::to_string(x * 5);
    } < task1;

    auto config =
        PipelineConfig().with_name("TaskReverse").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(task2);

    auto output = pipeline.execute();

    std::string result = task2->get<std::string>();
    CHECK(result == "40");
}

TEST_CASE("Task - tap() for logging without transformation") {
    std::vector<int> log;

    auto task1 =
        make_task([](CoroScope&) -> CoroTask<int> { co_return 123; }, "Source");

    auto task2 = task1->tap(
        [&log](CoroScope&, const std::any& input) -> CoroTask<void> {
            int value = std::any_cast<int>(input);
            log.push_back(value);
            co_return;
        },
        "Logger");

    auto task3 = task2->then(
        [](CoroScope&, int x) -> CoroTask<int> { co_return x * 2; }, "Doubler");

    auto config = PipelineConfig().with_name("TaskTap").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(task3);

    auto output = pipeline.execute();

    int result = task3->get<int>();
    CHECK(result == 246);
    CHECK(log.size() == 1);
    CHECK(log[0] == 123);
}

TEST_CASE("Task - Complex chaining with tap and then") {
    std::vector<std::string> log;

    auto task1 =
        make_task([](CoroScope&) -> CoroTask<int> { co_return 3; }, "Start");

    auto pipeline_task =
        task1
            ->tap(
                [&log](CoroScope&, const std::any& input) -> CoroTask<void> {
                    int val = std::any_cast<int>(input);
                    log.push_back("tap1: " + std::to_string(val));
                    co_return;
                },
                "Log1")
            ->then([](CoroScope&, int x) -> CoroTask<int> { co_return x + 7; },
                   "Add7")
            ->tap(
                [&log](CoroScope&, const std::any& input) -> CoroTask<void> {
                    int val = std::any_cast<int>(input);
                    log.push_back("tap2: " + std::to_string(val));
                    co_return;
                },
                "Log2")
            ->then([](CoroScope&, int x) -> CoroTask<int> { co_return x * 10; },
                   "Multiply10")
            ->tap(
                [&log](CoroScope&, const std::any& input) -> CoroTask<void> {
                    int val = std::any_cast<int>(input);
                    log.push_back("tap3: " + std::to_string(val));
                    co_return;
                },
                "Log3");

    auto config =
        PipelineConfig().with_name("ComplexChain").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(pipeline_task);

    auto output = pipeline.execute();

    int result = pipeline_task->get<int>();
    CHECK(result == 100);  // (3 + 7) * 10
    CHECK(log.size() == 3);
    CHECK(log[0] == "tap1: 3");
    CHECK(log[1] == "tap2: 10");
    CHECK(log[2] == "tap3: 100");
}

// ============================================================================
// Mixed CoroTask and Task Combinator Tests
// ============================================================================

TEST_CASE("Mixed - CoroTask combinators within Task") {
    std::atomic<int> checkpoint{0};

    auto task = make_task(
        [&checkpoint](CoroScope&) -> CoroTask<int> {
            checkpoint = 1;

            // Use CoroTask combinators inside a Task
            auto compute = []() -> CoroTask<int> { co_return 15; };

            int result =
                co_await (compute()
                              .tap([&checkpoint](int) { checkpoint = 2; })
                              .then([](int x) { return x + 5; })
                              .tap([&checkpoint](int) { checkpoint = 3; })
                              .then([](int x) { return x * 2; }));

            checkpoint = 4;
            co_return result;
        },
        "MixedTask");

    auto config = PipelineConfig().with_name("Mixed").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 40);  // (15 + 5) * 2
    CHECK(checkpoint.load() == 4);
}

TEST_CASE("Mixed - Task chaining with internal CoroTask operations") {
    std::vector<std::string> log;

    auto task1 = make_task(
        [&log](CoroScope&) -> CoroTask<int> {
            log.push_back("task1_start");

            auto inner = []() -> CoroTask<int> { co_return 10; };

            int val = co_await (inner().then([](int x) { return x + 5; }));

            log.push_back("task1_end");
            co_return val;
        },
        "Task1");

    auto task2 = task1->then(
        [&log](CoroScope&, int x) -> CoroTask<int> {
            log.push_back("task2_start");

            auto inner = [x]() -> CoroTask<int> { co_return x * 2; };

            int val = co_await (inner().tap([&log](int v) {
                log.push_back("task2_tap: " + std::to_string(v));
            }));

            log.push_back("task2_end");
            co_return val;
        },
        "Task2");

    auto config =
        PipelineConfig().with_name("MixedChain").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(task2);

    auto output = pipeline.execute();

    int result = task2->get<int>();
    CHECK(result == 30);  // (10 + 5) * 2
    CHECK(log.size() == 5);
    CHECK(log[0] == "task1_start");
    CHECK(log[1] == "task1_end");
    CHECK(log[2] == "task2_start");
    CHECK(log[3] == "task2_tap: 30");
    CHECK(log[4] == "task2_end");
}

// ============================================================================
// Performance and Stress Tests
// ============================================================================

TEST_CASE("Performance - Long chain of then() operations") {
    auto task = make_task(
        []() -> CoroTask<int> {
            auto compute = []() -> CoroTask<int> { co_return 1; };

            // Long chain
            auto result = co_await (compute()
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; })
                                        .then([](int x) { return x + 1; }));

            co_return result;
        },
        "LongChain");

    auto config =
        PipelineConfig().with_name("LongChainTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 11);  // 1 + 10
}

TEST_CASE("Performance - Multiple tap() operations") {
    std::atomic<int> tap_count{0};

    auto task = make_task(
        [&tap_count]() -> CoroTask<int> {
            auto compute = []() -> CoroTask<int> { co_return 42; };

            auto result =
                co_await (compute()
                              .tap([&tap_count](int) { tap_count++; })
                              .tap([&tap_count](int) { tap_count++; })
                              .tap([&tap_count](int) { tap_count++; })
                              .tap([&tap_count](int) { tap_count++; })
                              .tap([&tap_count](int) { tap_count++; }));

            co_return result;
        },
        "MultiTap");

    auto config =
        PipelineConfig().with_name("MultiTapTest").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 42);
    CHECK(tap_count.load() == 5);
}

TEST_CASE("Stress - Task DAG with combinators") {
    std::atomic<int> completed{0};

    auto task1 = make_task(
        [&completed](CoroScope&) -> CoroTask<int> {
            completed++;
            co_return 10;
        },
        "Task1");

    auto task2 = task1->then(
        [&completed](CoroScope&, int x) -> CoroTask<int> {
            completed++;
            co_return x + 5;
        },
        "Task2");

    auto task3 = task1->then(
        [&completed](CoroScope&, int x) -> CoroTask<int> {
            completed++;
            co_return x * 2;
        },
        "Task3");

    auto task4 = make_task(
        [&completed](CoroScope&, int a, int b) -> CoroTask<int> {
            completed++;
            co_return a + b;
        },
        "Task4");

    task4->depends_on(task2);
    task4->depends_on(task3);

    auto config =
        PipelineConfig().with_name("StressDAG").with_compute_threads(4);
    Pipeline pipeline(config);

    pipeline.set_source(task1);
    pipeline.set_destination(task4);

    auto output = pipeline.execute();

    int result = task4->get<int>();
    CHECK(result == 35);  // (10 + 5) + (10 * 2)
    CHECK(completed.load() == 4);
}

// ============================================================================
// Operator& (AND/Parallel) Tests
// ============================================================================

TEST_CASE("CoroTask - operator& parallel composition") {
    auto task = make_task(
        []() -> CoroTask<int> {
            auto compute1 = []() -> CoroTask<int> { co_return 10; };

            auto compute2 = []() -> CoroTask<std::string> {
                co_return std::string("hello");
            };

            // Use operator& to combine both
            auto [val1, val2] = co_await (compute1() & compute2());

            co_return val1 + static_cast<int>(val2.length());
        },
        "AndTest");

    auto config = PipelineConfig().with_name("AndOp").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 15);  // 10 + 5 (length of "hello")
}

TEST_CASE("CoroTask<void> - operator& with value task") {
    std::atomic<bool> side_effect{false};

    auto task = make_task(
        [&side_effect]() -> CoroTask<int> {
            auto void_work = [&side_effect]() -> CoroTask<void> {
                side_effect = true;
                co_return;
            };

            auto compute = []() -> CoroTask<int> { co_return 42; };

            // Combine void task with value task
            int result = co_await (void_work() & compute());
            co_return result;
        },
        "VoidAndTest");

    auto config = PipelineConfig().with_name("VoidAnd").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 42);
    CHECK(side_effect.load() == true);
}

TEST_CASE("Task - operator& parallel composition") {
    auto task1 =
        make_task([](CoroScope&) -> CoroTask<int> { co_return 20; }, "Task1");

    auto task2 =
        make_task([](CoroScope&) -> CoroTask<int> { co_return 22; }, "Task2");

    // Use operator& to create combiner
    auto combined = task1 & task2;

    auto config = PipelineConfig().with_name("TaskAnd").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source({task1, task2});
    pipeline.set_destination(combined);

    auto output = pipeline.execute();

    // Combined result is a tuple wrapped in std::any
    auto result = combined->get<std::tuple<std::any, std::any>>();
    int val1 = std::any_cast<int>(std::get<0>(result));
    int val2 = std::any_cast<int>(std::get<1>(result));

    CHECK(val1 == 20);
    CHECK(val2 == 22);
}

// ============================================================================
// Operator| (OR/Fallback) Tests
// ============================================================================

TEST_CASE("CoroTask - operator| fallback on success") {
    auto task = make_task(
        []() -> CoroTask<int> {
            auto primary = []() -> CoroTask<int> { co_return 99; };

            auto fallback = []() -> CoroTask<int> { co_return 0; };

            // Primary succeeds, fallback not executed
            int result = co_await (primary() | fallback());
            co_return result;
        },
        "OrSuccessTest");

    auto config =
        PipelineConfig().with_name("OrSuccess").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 99);
}

TEST_CASE("CoroTask - operator| fallback on failure") {
    auto task = make_task(
        []() -> CoroTask<int> {
            auto primary = []() -> CoroTask<int> {
                throw std::runtime_error("Primary failed");
                co_return 0;
            };

            auto fallback = []() -> CoroTask<int> { co_return 42; };

            // Primary fails, fallback executes
            int result = co_await (primary() | fallback());
            co_return result;
        },
        "OrFallbackTest");

    auto config =
        PipelineConfig().with_name("OrFallback").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 42);
}

TEST_CASE("CoroTask<void> - operator| fallback") {
    std::atomic<int> execution_count{0};

    auto task = make_task(
        [&execution_count]() -> CoroTask<void> {
            auto primary = [&execution_count]() -> CoroTask<void> {
                execution_count++;
                throw std::runtime_error("Primary void task failed");
                co_return;
            };

            auto fallback = [&execution_count]() -> CoroTask<void> {
                execution_count++;
                co_return;
            };

            // Primary fails, fallback executes
            co_await (primary() | fallback());
        },
        "VoidOrTest");

    auto config = PipelineConfig().with_name("VoidOr").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    CHECK(execution_count.load() == 2);  // Both executed
}

// ============================================================================
// Combined Operators Tests
// ============================================================================

TEST_CASE("CoroTask - Combining & and | operators") {
    auto task = make_task(
        []() -> CoroTask<int> {
            auto compute1 = []() -> CoroTask<int> { co_return 10; };

            auto compute2_primary = []() -> CoroTask<int> {
                throw std::runtime_error("Compute2 failed");
                co_return 0;
            };

            auto compute2_fallback = []() -> CoroTask<int> { co_return 5; };

            // Combine: compute1 AND (compute2_primary OR compute2_fallback)
            auto [val1, val2] = co_await (
                compute1() & (compute2_primary() | compute2_fallback()));

            co_return val1 + val2;
        },
        "CombinedOps");

    auto config =
        PipelineConfig().with_name("Combined").with_compute_threads(2);
    Pipeline pipeline(config);

    pipeline.set_source(task);
    auto output = pipeline.execute();

    int result = task->get<int>();
    CHECK(result == 15);  // 10 + 5 (fallback)
}
