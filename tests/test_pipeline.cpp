#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <doctest/doctest.h>

#include <any>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;

// ============================================================================
// Basic Scheduler Tests
// ============================================================================

TEST_CASE("Scheduler - Basic construction and destruction") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    // Should construct and destruct cleanly
    CHECK(scheduler.get_watchdog() == nullptr);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Scheduler - Construction with config") {
    Executor executor(4);

    auto config = PipelineConfigManager::default_config();
    Scheduler scheduler(&executor, config);

    // Should have watchdog enabled by default
    CHECK(scheduler.get_watchdog() != nullptr);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Scheduler - Sequential config") {
    Executor executor(1);

    auto config = PipelineConfigManager::sequential();
    Scheduler scheduler(&executor, config);

    // Sequential mode should disable watchdog
    CHECK(scheduler.get_watchdog() == nullptr);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Scheduler - Parallel config") {
    Executor executor(4);

    auto config = PipelineConfigManager::parallel(4);
    Scheduler scheduler(&executor, config);

    // Parallel mode should enable watchdog
    CHECK(scheduler.get_watchdog() != nullptr);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

// ============================================================================
// Fluent API Tests
// ============================================================================

TEST_CASE("PipelineConfigManager - Fluent API basic") {
    auto config =
        PipelineConfigManager().with_executor_threads(4).with_watchdog(true);

    CHECK(config.executor_threads == 4);
    CHECK(config.enable_watchdog == true);
}

TEST_CASE("PipelineConfigManager - Fluent API with timeouts") {
    auto config = PipelineConfigManager()
                      .with_executor_threads(4)
                      .with_global_timeout(std::chrono::seconds(30))
                      .with_task_timeout(std::chrono::seconds(10));

    CHECK(config.executor_threads == 4);
    CHECK(config.global_timeout == std::chrono::seconds(30));
    CHECK(config.default_task_timeout == std::chrono::seconds(10));
}

TEST_CASE("PipelineConfigManager - Fluent API chaining") {
    auto config = PipelineConfigManager()
                      .with_executor_threads(8)
                      .with_watchdog(true)
                      .with_global_timeout(std::chrono::minutes(1))
                      .with_task_timeout(std::chrono::seconds(30))
                      .with_watchdog_interval(std::chrono::milliseconds(50))
                      .with_warning_threshold(std::chrono::seconds(5));

    CHECK(config.executor_threads == 8);
    CHECK(config.enable_watchdog == true);
    CHECK(config.global_timeout == std::chrono::minutes(1));
    CHECK(config.default_task_timeout == std::chrono::seconds(30));
    CHECK(config.watchdog_interval == std::chrono::milliseconds(50));
    CHECK(config.long_task_warning_threshold == std::chrono::seconds(5));
}

TEST_CASE("PipelineConfigManager - Static factory methods") {
    SUBCASE("default_config") {
        auto config = PipelineConfigManager::default_config();
        CHECK(config.executor_threads == 0);  // hardware_concurrency
        CHECK(config.enable_watchdog == true);
    }

    SUBCASE("sequential") {
        auto config = PipelineConfigManager::sequential();
        CHECK(config.executor_threads == 1);
        CHECK(config.enable_watchdog == false);
    }

    SUBCASE("parallel") {
        auto config = PipelineConfigManager::parallel(4);
        CHECK(config.executor_threads == 4);
        CHECK(config.enable_watchdog == true);
    }

    SUBCASE("with_timeouts") {
        auto config = PipelineConfigManager::with_timeouts(
            4, std::chrono::seconds(60), std::chrono::seconds(30));
        CHECK(config.executor_threads == 4);
        CHECK(config.enable_watchdog == true);
        CHECK(config.global_timeout == std::chrono::seconds(60));
        CHECK(config.default_task_timeout == std::chrono::seconds(30));
    }
}

// ============================================================================
// Task Scheduling Tests
// ============================================================================

TEST_CASE("Scheduler - Schedule simple task") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};

    auto task = make_task(
        [&counter]() -> int {
            counter++;
            return 42;
        },
        "SimpleTask");

    scheduler.schedule(task);

    // Wait for task to complete
    task->get<int>();

    CHECK(counter.load() == 1);
    CHECK(task->is_completed());

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Scheduler - Task dependencies") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::vector<int> execution_order;
    std::mutex order_mutex;

    auto task1 = make_task(
        [&]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        },
        "Task1");

    auto task2 = make_task(
        [&]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
        },
        "Task2");

    // Task2 depends on Task1
    task2->depends_on(task1);

    scheduler.schedule(task1);

    // Wait for completion
    task2->wait();

    // Task1 should execute before Task2
    CHECK(execution_order.size() == 2);
    CHECK(execution_order[0] == 1);
    CHECK(execution_order[1] == 2);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

// ============================================================================
// Threading Tests
// ============================================================================

TEST_CASE("Scheduler - Threading with multiple workers") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> active_count{0};
    std::atomic<int> max_active{0};
    std::atomic<int> completed{0};

    // Create a root task
    auto root_task = make_task(
        [&]() {
            int current = ++active_count;
            int current_max = max_active.load();
            while (current > current_max &&
                   !max_active.compare_exchange_weak(current_max, current)) {
                current_max = max_active.load();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            --active_count;
            ++completed;
        },
        "RootTask");

    // Create child tasks
    std::shared_ptr<Task> last_child;
    for (int i = 1; i < 20; ++i) {
        auto child = make_task(
            [&]() {
                int current = ++active_count;
                int current_max = max_active.load();
                while (
                    current > current_max &&
                    !max_active.compare_exchange_weak(current_max, current)) {
                    current_max = max_active.load();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                --active_count;
                ++completed;
            },
            "Task_" + std::to_string(i));
        child->depends_on(root_task);
        last_child = child;
    }

    scheduler.schedule(root_task);

    // Wait for all tasks to complete
    if (last_child) {
        last_child->wait();
    }

    CHECK(completed.load() == 20);
    // Should have some parallelism (at least 2 tasks running concurrently)
    CHECK(max_active.load() >= 2);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}
TEST_CASE("Scheduler - Single threaded execution") {
    Executor executor(1);
    auto config = PipelineConfigManager::sequential();
    Scheduler scheduler(&executor, config);

    std::atomic<int> active_count{0};
    std::atomic<int> max_active{0};

    auto root_task = make_task(
        [&]() {
            int current = ++active_count;
            int current_max = max_active.load();
            while (current > current_max &&
                   !max_active.compare_exchange_weak(current_max, current)) {
                current_max = max_active.load();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            --active_count;
        },
        "RootTask");

    std::shared_ptr<Task> last_child;
    for (int i = 1; i < 10; ++i) {
        auto child = make_task(
            [&]() {
                int current = ++active_count;
                int current_max = max_active.load();
                while (
                    current > current_max &&
                    !max_active.compare_exchange_weak(current_max, current)) {
                    current_max = max_active.load();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                --active_count;
            },
            "Task_" + std::to_string(i));
        child->depends_on(root_task);
        last_child = child;
    }

    scheduler.schedule(root_task);

    // Wait for completion
    if (last_child) {
        last_child->wait();
    }

    // Should only ever have 1 task active at a time
    CHECK(max_active.load() == 1);
}
// ============================================================================
// Timeout Tests
// ============================================================================

TEST_CASE("Scheduler - Global timeout triggers") {
    // Use a very short timeout to make test fast and deterministic
    auto config = PipelineConfigManager()
                      .with_executor_threads(4)
                      .with_global_timeout(std::chrono::milliseconds(50))
                      .with_watchdog(true)
                      .with_watchdog_interval(std::chrono::milliseconds(10));

    // Create executor and scheduler in a scope so they cleanup properly
    {
        Executor executor(4);
        Scheduler scheduler(&executor, config);

        // Create a task that spins/blocks longer than timeout
        // Use shared_ptr to ensure flag survives task execution
        auto should_exit = std::make_shared<std::atomic<bool>>(false);
        auto task_started = std::make_shared<std::atomic<bool>>(false);

        auto blocking_task = make_task(
            [should_exit, task_started]() {
                task_started->store(true);
                // Busy-wait loop that checks flag periodically
                // This is more deterministic than sleep for CI
                auto start = std::chrono::steady_clock::now();
                while (!should_exit->load()) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    // Exit after 2 seconds max (way longer than timeout)
                    if (elapsed > std::chrono::seconds(2)) {
                        break;
                    }
                    // Use yield to be less CPU-intensive but still responsive
                    for (int i = 0; i < 1000; ++i) {
                        std::this_thread::yield();
                    }
                }
            },
            "BlockingTask");

        // Should throw timeout error
        bool caught_timeout = false;
        try {
            scheduler.schedule(blocking_task);
        } catch (const PipelineError& e) {
            caught_timeout = (e.get_type() == PipelineError::TIMEOUT_ERROR);
        }

        // Cleanup: signal task to exit and give it time
        should_exit->store(true);

        // Wait briefly for task to exit
        auto cleanup_start = std::chrono::steady_clock::now();
        while (task_started->load() &&
               (std::chrono::steady_clock::now() - cleanup_start <
                std::chrono::milliseconds(500))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        CHECK(caught_timeout);
    }

    // Give time for executor cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("Scheduler - No timeout with zero value") {
    Executor executor(4);
    auto config = PipelineConfigManager()
                      .with_executor_threads(4)
                      .with_global_timeout(
                          std::chrono::milliseconds(0))  // 0 = wait forever
                      .with_watchdog(false);

    Scheduler scheduler(&executor, config);

    std::atomic<bool> completed{false};

    auto task = make_task(
        [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            completed = true;
        },
        "Task");

    scheduler.schedule(task);

    // Wait for task to complete
    task->wait();

    // Should complete without timeout
    CHECK(completed.load() == true);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

// ============================================================================
// Watchdog Tests
// ============================================================================

TEST_CASE("Watchdog - Basic construction") {
    Watchdog watchdog(std::chrono::milliseconds(100));

    // Should construct cleanly
    CHECK_NOTHROW(watchdog.start());
    CHECK_NOTHROW(watchdog.stop());
}

TEST_CASE("Watchdog - Construction with parameters") {
    Watchdog watchdog(std::chrono::milliseconds(50),    // check_interval
                      std::chrono::milliseconds(5000),  // global_timeout
                      std::chrono::milliseconds(1000),  // default_task_timeout
                      std::chrono::milliseconds(500)    // warning_threshold
    );

    CHECK_NOTHROW(watchdog.start());
    CHECK_NOTHROW(watchdog.stop());
}

// ============================================================================
// Shutdown Tests
// ============================================================================

TEST_CASE("Scheduler - Graceful shutdown") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> completed{0};
    std::atomic<bool> tasks_started{false};

    // Create a task with fewer children and shorter sleep to reduce flakiness
    auto root_task = make_task(
        [&]() {
            completed++;
            tasks_started = true;
        },
        "RootTask");

    // Store children to prevent them from being destroyed
    std::vector<std::shared_ptr<Task>> children;
    for (int i = 1; i < 20; ++i) {  // Reduced from 100 to 20
        auto child = make_task(
            [&]() {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));  // Reduced from 50 to 10
                completed++;
            },
            "Task_" + std::to_string(i));
        child->depends_on(root_task);
        children.push_back(child);
    }

    // Start scheduling in a separate thread
    std::thread scheduler_thread([&]() {
        try {
            scheduler.schedule(root_task);
        } catch (const PipelineError&) {
            // Expected if shutdown is requested
        }
    });

    // Wait for tasks to actually start (more reliable than fixed sleep)
    for (int i = 0; i < 100 && !tasks_started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Request shutdown
    scheduler.request_shutdown();

    // Wait for scheduler to finish
    scheduler_thread.join();

    // Verify shutdown was requested and at least root task completed
    CHECK(scheduler.is_shutdown_requested());
    CHECK(tasks_started.load());  // Root task should have started
}
TEST_CASE("Scheduler - Shutdown during execution") {
    Executor executor(2);
    auto config =
        PipelineConfigManager().with_executor_threads(2).with_watchdog(false);

    Scheduler scheduler(&executor, config);

    std::atomic<bool> task_running{false};

    auto task = make_task(
        [&]() {
            task_running = true;
            for (int i = 0; i < 100; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        },
        "LongRunningTask");

    std::thread scheduler_thread([&]() {
        try {
            scheduler.schedule(task);
        } catch (...) {
            // May throw if interrupted
        }
    });

    // Wait for task to start
    while (!task_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Request shutdown while task is running
    scheduler.request_shutdown();

    scheduler_thread.join();

    CHECK(scheduler.is_shutdown_requested());
}
// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("Integration - Scheduler with Watchdog and Timeout") {
    Executor executor(4);
    // Increase timeout to 2 seconds to account for CI overhead and scheduling
    // delays
    auto config = PipelineConfigManager::with_timeouts(
        4, std::chrono::seconds(5), std::chrono::seconds(2));

    Scheduler scheduler(&executor, config);

    std::atomic<int> completed{0};

    auto root_task = make_task([&]() { completed++; }, "RootTask");

    // Create children with varying execution times (reduced to avoid flakiness
    // in CI)
    std::shared_ptr<Task> last_child;
    for (int i = 1; i < 10; ++i) {
        auto child = make_task(
            [&, i]() {
                // Reduced sleep times to minimize timing sensitivity
                int sleep_ms = (i % 3 == 0) ? 50 : 10;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(sleep_ms));
                completed++;
            },
            "MixedTask_" + std::to_string(i));
        child->depends_on(root_task);
        last_child = child;
    }

    // Should complete without timeout
    CHECK_NOTHROW(scheduler.schedule(root_task));

    // Wait for all tasks to complete
    if (last_child) {
        last_child->wait();
    }

    CHECK(completed.load() == 10);
}
TEST_CASE("Integration - Full pipeline with all features") {
    Executor executor(4);
    auto config = PipelineConfigManager()
                      .with_executor_threads(4)
                      .with_watchdog(true)
                      .with_global_timeout(std::chrono::seconds(5))
                      .with_task_timeout(std::chrono::seconds(1))
                      .with_watchdog_interval(std::chrono::milliseconds(50))
                      .with_warning_threshold(std::chrono::milliseconds(500));

    Scheduler scheduler(&executor, config);

    // Create a dependency chain
    auto task1 = make_task(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); },
        "ChainTask_0");

    auto task2 = make_task(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); },
        "ChainTask_1");

    auto task3 = make_task(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); },
        "ChainTask_2");

    task2->depends_on(task1);
    task3->depends_on(task2);

    // Should complete successfully
    CHECK_NOTHROW(scheduler.schedule(task1));

    // Wait for all tasks to complete
    task3->wait();

    // All tasks should be completed
    CHECK(task1->is_completed());
    CHECK(task2->is_completed());
    CHECK(task3->is_completed());

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("Error Types - All error types present") {
    CHECK(PipelineError::TYPE_MISMATCH >= 0);
    CHECK(PipelineError::VALIDATION_ERROR >= 0);
    CHECK(PipelineError::EXECUTION_ERROR >= 0);
    CHECK(PipelineError::INITIALIZATION_ERROR >= 0);
    CHECK(PipelineError::OUTPUT_CONVERSION_ERROR >= 0);
    CHECK(PipelineError::TIMEOUT_ERROR >= 0);
    CHECK(PipelineError::INTERRUPTED >= 0);
    CHECK(PipelineError::EXECUTOR_UNRESPONSIVE >= 0);
}

TEST_CASE("Error - Timeout error message") {
    PipelineError error(PipelineError::TIMEOUT_ERROR, "Pipeline timed out");
    std::string msg = error.what();

    CHECK(msg.find("TIMEOUT") != std::string::npos);
    CHECK(msg.find("Pipeline timed out") != std::string::npos);
}

TEST_CASE("Error - Interrupted error message") {
    PipelineError error(PipelineError::INTERRUPTED, "Pipeline interrupted");
    std::string msg = error.what();

    CHECK(msg.find("INTERRUPTED") != std::string::npos);
    CHECK(msg.find("Pipeline interrupted") != std::string::npos);
}

TEST_CASE("Error - Executor unresponsive error message") {
    PipelineError error(PipelineError::EXECUTOR_UNRESPONSIVE, "Executor hung");
    std::string msg = error.what();

    CHECK(msg.find("EXECUTOR_UNRESPONSIVE") != std::string::npos);
    CHECK(msg.find("Executor hung") != std::string::npos);
}

// ============================================================================
// Error Scenario Tests - Comprehensive
// ============================================================================

TEST_CASE("Error Scenario - Task throws exception") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    auto throwing_task = make_task(
        []() { throw std::runtime_error("Task failed intentionally"); },
        "ThrowingTask");

    // FAIL_FAST policy should throw immediately
    scheduler.set_error_policy(ErrorPolicy::FAIL_FAST);

    CHECK_THROWS_AS(scheduler.schedule(throwing_task), PipelineError);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Error Scenario - Task throws exception in chain") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> completed_count{0};

    auto task1 = make_task([&]() { ++completed_count; }, "Task1");

    auto task2 = make_task(
        [&]() {
            ++completed_count;
            throw std::runtime_error("Task2 failed");
        },
        "Task2_Throws");

    auto task3 = make_task([&]() { ++completed_count; }, "Task3");

    task2->depends_on(task1);
    task3->depends_on(task2);

    scheduler.set_error_policy(ErrorPolicy::FAIL_FAST);

    CHECK_THROWS_AS(scheduler.schedule(task1), PipelineError);

    // Task1 should have completed, Task2 threw, Task3 should not run
    CHECK(completed_count.load() >= 1);
    CHECK(completed_count.load() <= 2);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Error Scenario - Type mismatch between tasks") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    // Task returns int
    auto int_task = make_task([]() -> int { return 42; }, "IntTask");

    // Task expects string input
    auto string_task = make_task(
        [](const std::string& s) -> int {
            return static_cast<int>(s.length());
        },
        "StringTask");

    // Should throw type mismatch error when setting up dependency
    bool caught_type_error = false;
    try {
        string_task->depends_on(int_task);

        // If we get here, try scheduling (might throw later)
        scheduler.schedule(int_task);
    } catch (const PipelineError& e) {
        caught_type_error =
            (e.get_type() == PipelineError::TYPE_MISMATCH ||
             e.get_type() == PipelineError::TYPE_MISMATCH_ERROR);
    } catch (const std::exception& e) {
        // Type checking might throw std::exception
        std::string msg = e.what();
        caught_type_error = (msg.find("TYPE_MISMATCH") != std::string::npos ||
                             msg.find("Type mismatch") != std::string::npos);
    }

    CHECK(caught_type_error);
}
TEST_CASE("Error Policy - FAIL_FAST stops on first error") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> tasks_started{0};
    std::atomic<int> tasks_completed{0};

    // Create parallel tasks, one will fail
    auto task1 = make_task(
        [&]() {
            ++tasks_started;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++tasks_completed;
        },
        "Task1");

    auto task2 = make_task(
        [&]() {
            ++tasks_started;
            throw std::runtime_error("Task2 fails");
        },
        "Task2_Fails");

    auto task3 = make_task(
        [&]() {
            ++tasks_started;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++tasks_completed;
        },
        "Task3");

    // All tasks are children of a root task
    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);
    task3->depends_on(root);

    scheduler.set_error_policy(ErrorPolicy::FAIL_FAST);

    CHECK_THROWS_AS(scheduler.schedule(root), PipelineError);

    // At least one task should have started (the failing one)
    CHECK(tasks_started.load() >= 1);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Error Policy - CONTINUE continues on error") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> tasks_completed{0};
    std::atomic<bool> task2_threw{false};

    auto task1 = make_task(
        [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++tasks_completed;
        },
        "Task1");

    auto task2 = make_task(
        [&]() {
            task2_threw = true;
            throw std::runtime_error("Task2 fails but should continue");
        },
        "Task2_Fails");

    auto task3 = make_task(
        [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++tasks_completed;
        },
        "Task3");

    // Parallel tasks from root
    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);
    task3->depends_on(root);

    scheduler.set_error_policy(ErrorPolicy::CONTINUE);

    // With CONTINUE policy, should not throw
    CHECK_NOTHROW(scheduler.schedule(root));

    // Wait for all tasks to complete
    task1->wait();
    task3->wait();

    // Task2 should have thrown, but task1 and task3 should complete
    CHECK(task2_threw.load());
    CHECK(tasks_completed.load() == 2);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Error Scenario - Per-task timeout") {
    auto config = PipelineConfigManager()
                      .with_executor_threads(4)
                      .with_watchdog(true)
                      .with_task_timeout(std::chrono::milliseconds(
                          150))  // Set default (increased for CI)
                      .with_watchdog_interval(std::chrono::milliseconds(25));

    {
        Executor executor(4);
        Scheduler scheduler(&executor, config);

        // Create task with specific timeout
        auto should_exit = std::make_shared<std::atomic<bool>>(false);
        auto task_started = std::make_shared<std::atomic<bool>>(false);

        auto task_with_timeout = make_task(
            [should_exit, task_started]() {
                task_started->store(true);
                auto start = std::chrono::steady_clock::now();
                while (!should_exit->load()) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > std::chrono::seconds(2)) {
                        break;
                    }
                    for (int i = 0; i < 1000; ++i) {
                        std::this_thread::yield();
                    }
                }
            },
            "TaskWithTimeout");

        // Set per-task timeout (shorter than the task duration)
        task_with_timeout->with_timeout(std::chrono::milliseconds(150));

        bool caught_timeout = false;
        try {
            scheduler.schedule(task_with_timeout);
        } catch (const PipelineError& e) {
            caught_timeout = (e.get_type() == PipelineError::TIMEOUT_ERROR);
        }

        should_exit->store(true);

        // Wait briefly for task to exit
        auto cleanup_start = std::chrono::steady_clock::now();
        while (task_started->load() &&
               (std::chrono::steady_clock::now() - cleanup_start <
                std::chrono::milliseconds(500))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        CHECK(caught_timeout);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
TEST_CASE("Error Scenario - Validation error on null task") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    // Trying to schedule null task should throw validation error
    bool caught_validation = false;
    try {
        scheduler.schedule(nullptr);
    } catch (const PipelineError& e) {
        caught_validation = (e.get_type() == PipelineError::VALIDATION_ERROR);
    }

    CHECK(caught_validation);
}
TEST_CASE("Error Scenario - Graceful shutdown (INTERRUPTED)") {
    auto config =
        PipelineConfigManager().with_executor_threads(4).with_watchdog(false);

    {
        Executor executor(4);
        Scheduler scheduler(&executor, config);

        auto should_exit = std::make_shared<std::atomic<bool>>(false);
        auto task_started = std::make_shared<std::atomic<bool>>(false);

        auto long_task = make_task(
            [should_exit, task_started]() {
                task_started->store(true);
                auto start = std::chrono::steady_clock::now();
                while (!should_exit->load()) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > std::chrono::seconds(2)) {
                        break;
                    }
                    for (int i = 0; i < 1000; ++i) {
                        std::this_thread::yield();
                    }
                }
            },
            "LongTask");

        // Run scheduler in separate thread
        std::atomic<bool> caught_interrupted{false};
        std::thread scheduler_thread([&]() {
            try {
                scheduler.schedule(long_task);
            } catch (const PipelineError& e) {
                caught_interrupted =
                    (e.get_type() == PipelineError::INTERRUPTED);
            }
        });

        // Give task time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Request shutdown
        scheduler.request_shutdown();

        // Wait for scheduler to finish
        scheduler_thread.join();

        should_exit->store(true);

        // Wait briefly for task to exit
        auto cleanup_start = std::chrono::steady_clock::now();
        while (task_started->load() &&
               (std::chrono::steady_clock::now() - cleanup_start <
                std::chrono::milliseconds(500))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        CHECK(caught_interrupted.load());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
// ============================================================================
// Pipeline Class Tests - High-Level API
// ============================================================================

TEST_CASE("Pipeline - Basic construction and validation") {
    auto config = PipelineConfigManager()
                      .with_name("TestPipeline")
                      .with_executor_threads(4);
    Pipeline pipeline(config);

    CHECK(pipeline.get_name() == "TestPipeline");
    CHECK(pipeline.get_source() == nullptr);
    CHECK(pipeline.get_destination() == nullptr);
}

TEST_CASE("Pipeline - Single task execution") {
    auto config = PipelineConfigManager()
                      .with_name("SingleTask")
                      .with_executor_threads(4);
    Pipeline pipeline(config);

    std::atomic<bool> executed{false};
    auto task = make_task([&]() { executed = true; }, "SingleTask");

    pipeline.set_source(task);
    CHECK(pipeline.validate());

    auto output = pipeline.execute();

    CHECK(executed.load());
    // If execute() returns without throwing, the pipeline succeeded
}

TEST_CASE("Pipeline - Task chain execution") {
    auto config =
        PipelineConfigManager().with_name("TaskChain").with_executor_threads(4);
    Pipeline pipeline(config);

    std::vector<int> execution_order;
    std::mutex order_mutex;

    auto task1 = make_task(
        [&]() -> int {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
            return 42;
        },
        "Task1");

    auto task2 = make_task(
        [&](int x) -> int {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
            return x * 2;
        },
        "Task2");

    auto task3 = make_task(
        [&](int x) -> int {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(3);
            return x + 10;
        },
        "Task3");

    task2->depends_on(task1);
    task3->depends_on(task2);

    pipeline.set_source(task1);
    pipeline.set_destination(task3);

    CHECK(pipeline.validate());

    auto output = pipeline.execute();

    CHECK(execution_order == std::vector<int>{1, 2, 3});

    auto result = task3->get<int>();
    CHECK(result == 94);  // (42 * 2) + 10
}

TEST_CASE("Pipeline - Multiple sources with add_sources") {
    auto config = PipelineConfigManager()
                      .with_name("MultiSource")
                      .with_executor_threads(4);
    Pipeline pipeline(config);

    std::atomic<int> count{0};

    auto task1 = make_task([&]() { ++count; }, "Source1");

    auto task2 = make_task([&]() { ++count; }, "Source2");

    auto task3 = make_task([&]() { ++count; }, "Source3");

    pipeline.add_sources({task1, task2, task3});

    CHECK(pipeline.validate());

    auto output = pipeline.execute();

    CHECK(count.load() == 3);
}

TEST_CASE("Pipeline - Error policy propagation") {
    auto config = PipelineConfigManager()
                      .with_name("ErrorPolicy")
                      .with_executor_threads(4);
    Pipeline pipeline(config);
    pipeline.set_error_policy(ErrorPolicy::FAIL_FAST);

    auto failing_task = make_task(
        []() { throw std::runtime_error("Task fails"); }, "FailingTask");

    pipeline.set_source(failing_task);
    CHECK(pipeline.validate());

    CHECK_THROWS_AS(pipeline.execute(), PipelineError);
}

TEST_CASE("Pipeline - Progress callback") {
    auto config = PipelineConfigManager()
                      .with_name("ProgressTracking")
                      .with_executor_threads(4);
    Pipeline pipeline(config);

    std::atomic<size_t> last_completed{0};
    std::atomic<size_t> last_total{0};

    pipeline.set_progress_callback([&](size_t completed, size_t total) {
        last_completed = completed;
        last_total = total;
    });

    auto task1 = make_task(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); },
        "Task1");

    auto task2 = make_task(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); },
        "Task2");

    task2->depends_on(task1);

    pipeline.set_source(task1);
    CHECK(pipeline.validate());

    auto output = pipeline.execute();

    CHECK(last_total.load() >= 2);
    CHECK(last_completed.load() >= 2);
}

// ============================================================================
// Combiner Function Tests
// ============================================================================

TEST_CASE("Combiner - Multiple parents dependency resolution") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> parent_count{0};
    std::atomic<int> child_count{0};

    auto task1 = make_task([&]() { ++parent_count; }, "Task1");

    auto task2 = make_task([&]() { ++parent_count; }, "Task2");

    auto task3 = make_task([&]() { ++parent_count; }, "Task3");

    // Child task with multiple parents - should wait for all
    auto child = make_task(
        [&]() {
            // Should only execute after all 3 parents complete
            child_count = parent_count.load();
        },
        "ChildCombiner");

    child->depends_on(task1);
    child->depends_on(task2);
    child->depends_on(task3);

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);
    task3->depends_on(root);

    scheduler.schedule(root);

    // Wait for child to complete
    child->wait();

    CHECK(parent_count.load() == 3);
    CHECK(child_count.load() == 3);  // Child should see all parents completed

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Combiner - Type-safe tuple-based multi-argument function") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> result{0};

    auto task1 = make_task([]() -> int { return 5; }, "Task1");

    auto task2 = make_task([]() -> int { return 7; }, "Task2");

    // Child task with type-safe multi-argument function!
    // No vector<any>, no manual casting - just clean typed args
    auto child = make_task(
        [&](int a, int b) {
            result = a * b;  // 5 * 7 = 35
        },
        "ChildMultiArg");

    child->depends_on(task1);
    child->depends_on(task2);

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);

    scheduler.schedule(root);

    // Wait for child to complete
    child->wait();

    CHECK(result.load() == 35);  // 5 * 7

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Combiner - Three argument function") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto task1 = make_task([]() -> int { return 10; }, "Task1");

    auto task2 = make_task([]() -> int { return 20; }, "Task2");

    auto task3 = make_task([]() -> int { return 30; }, "Task3");

    // Three typed arguments - no casting needed!
    auto child = make_task([&](int a, int b, int c) { sum = a + b + c; },
                           "ChildThreeArgs");

    child->depends_on(task1);
    child->depends_on(task2);
    child->depends_on(task3);

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);
    task3->depends_on(root);

    scheduler.schedule(root);

    // Wait for child to complete
    child->wait();

    CHECK(sum.load() == 60);  // 10 + 20 + 30

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Combiner - with_combiner using vector<any>") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> sum{0};

    auto task1 = make_task([]() -> int { return 10; }, "Task1");
    auto task2 = make_task([]() -> int { return 20; }, "Task2");
    auto task3 = make_task([]() -> int { return 30; }, "Task3");

    // Use with_combiner to manually extract and combine parent outputs
    auto child =
        make_task([&](int combined) { sum = combined; }, "ChildWithCombiner");

    child->with_combiner([](const std::vector<std::any>& inputs) {
        int result = 0;
        for (const auto& input : inputs) {
            result += std::any_cast<int>(input);
        }
        return result;
    });

    child->depends_on({task1, task2, task3});

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);
    task3->depends_on(root);

    scheduler.schedule(root);

    // Wait for child to complete
    child->wait();

    CHECK(sum.load() == 60);  // 10 + 20 + 30
}
TEST_CASE("Combiner - with_combiner using std::function with typed args") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> product{0};

    auto task1 = make_task([]() -> int { return 5; }, "Task1");
    auto task2 = make_task([]() -> int { return 7; }, "Task2");

    // Use with_combiner with std::function that takes specific types
    auto child = make_task([&](int combined) { product = combined; },
                           "ChildWithTypedCombiner");

    child->with_combiner([](int a, int b) { return a * b; });

    child->depends_on({task1, task2});

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);

    scheduler.schedule(root);

    // Wait for child to complete
    child->wait();

    CHECK(product.load() == 35);  // 5 * 7

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Combiner - with_combiner validation error") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    auto task1 = make_task([]() -> int { return 10; }, "Task1");
    auto task2 = make_task([]() -> int { return 20; }, "Task2");

    // Create combiner that expects 3 inputs but only has 2 parents
    auto child = make_task(
        [&](int /*val*/) {
            // Should not reach here
        },
        "ChildBadCombiner");

    child->with_combiner([](int a, int b, int c) { return a + b + c; });

    child->depends_on({task1, task2});

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);

    bool threw_error = false;
    try {
        scheduler.schedule(root);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // The error should be stored in the future - trying to get() should
        // throw
        child->get<int>();
    } catch (const dftracer::utils::PipelineError& e) {
        threw_error = true;
        // Verify it's a combiner validation error
        std::string msg = e.what();
        CHECK((msg.find("Combiner expects 3") != std::string::npos ||
               msg.find("Pipeline execution failed") != std::string::npos));
    } catch (const std::exception& e) {
        threw_error = true;
    }

    CHECK(threw_error);
}
// ============================================================================
// Complex DAG Structure Tests
// ============================================================================

TEST_CASE("DAG - Diamond pattern") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::vector<int> execution_order;
    std::mutex order_mutex;

    auto root = make_task(
        [&]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(0);
        },
        "Root");

    auto left = make_task(
        [&]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
        },
        "Left");

    auto right = make_task(
        [&]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
        },
        "Right");

    auto bottom = make_task(
        [&]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(3);
        },
        "Bottom");

    left->depends_on(root);
    right->depends_on(root);
    bottom->depends_on(left);
    bottom->depends_on(right);

    scheduler.schedule(root);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CHECK(execution_order.size() == 4);
    CHECK(execution_order[0] == 0);  // Root first
    CHECK(execution_order[3] == 3);  // Bottom last

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
    // Left and right can execute in any order
}

TEST_CASE("DAG - Multiple branches converging") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> final_count{0};

    auto root = make_task([]() {}, "Root");

    // Create 5 parallel branches
    std::vector<std::shared_ptr<Task>> branches;
    for (int i = 0; i < 5; ++i) {
        auto task = make_task(
            []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            },
            "Branch_" + std::to_string(i));
        task->depends_on(root);
        branches.push_back(task);
    }

    // Final task depends on all branches
    auto final_task = make_task([&]() { ++final_count; }, "FinalTask");

    for (const auto& branch : branches) {
        final_task->depends_on(branch);
    }

    scheduler.schedule(root);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    CHECK(final_count.load() == 1);
}
TEST_CASE("DAG - Wide and deep structure") {
    Executor executor(8);
    Scheduler scheduler(&executor);

    std::atomic<int> completed{0};

    auto root = make_task([]() {}, "Root");

    // Create 3 levels with branching
    std::vector<std::shared_ptr<Task>> level1, level2, level3;

    // Level 1: 4 tasks from root
    for (int i = 0; i < 4; ++i) {
        auto task = make_task(
            [&]() {
                ++completed;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            },
            "L1_" + std::to_string(i));
        task->depends_on(root);
        level1.push_back(task);
    }

    // Level 2: 8 tasks, 2 from each level1 task
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 2; ++j) {
            auto task = make_task(
                [&]() {
                    ++completed;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                },
                "L2_" + std::to_string(i * 2 + j));
            task->depends_on(level1[i]);
            level2.push_back(task);
        }
    }

    // Level 3: 4 tasks, each depends on 2 level2 tasks
    for (int i = 0; i < 4; ++i) {
        auto task =
            make_task([&]() { ++completed; }, "L3_" + std::to_string(i));
        task->depends_on(level2[i * 2]);
        task->depends_on(level2[i * 2 + 1]);
        level3.push_back(task);
    }

    scheduler.schedule(root);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    CHECK(completed.load() == 16);  // 4 + 8 + 4
}
// ============================================================================
// Dynamic Task Submission Tests
// ============================================================================

TEST_CASE("Dynamic Tasks - Task submits child task at runtime") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> total_tasks{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) {
            ++total_tasks;

            // Dynamically create and submit a child task
            auto child = make_task([&]() { ++total_tasks; }, "DynamicChild");

            ctx.submit_task(child);
        },
        "ParentTask");

    scheduler.schedule(parent_task);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CHECK(total_tasks.load() == 2);

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Dynamic Tasks - Multiple dynamic children") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> total_tasks{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) {
            ++total_tasks;

            // Create 5 dynamic children
            for (int i = 0; i < 5; ++i) {
                auto child = make_task(
                    [&]() {
                        ++total_tasks;
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(20));
                    },
                    "DynamicChild_" + std::to_string(i));

                ctx.submit_task(child);
            }
        },
        "ParentTask");

    scheduler.schedule(parent_task);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    CHECK(total_tasks.load() == 6);  // 1 parent + 5 children
}
TEST_CASE("Dynamic Tasks - Intra-task parallelism with result aggregation") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> final_result{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) {
            // Spawn multiple parallel child tasks and wait for their results
            std::vector<TaskFuture> futures;

            // Create 5 child tasks that compute values in parallel
            for (int i = 0; i < 5; ++i) {
                auto child = make_task(
                    [i]() -> int {
                        // Simulate some work
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                        return (i + 1) * 10;  // Returns 10, 20, 30, 40, 50
                    },
                    "ChildTask_" + std::to_string(i));

                // Submit and collect future
                futures.push_back(ctx.submit_task(child));
            }

            // Wait for all children and aggregate results
            int sum = 0;
            for (auto& future : futures) {
                int value = future.get<int>();
                sum += value;
            }

            final_result = sum;
            return sum;
        },
        "ParentTaskWithAggregation");

    scheduler.schedule(parent_task);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Should have computed: 10 + 20 + 30 + 40 + 50 = 150
    CHECK(final_result.load() == 150);
}
TEST_CASE("Dynamic Tasks - Nested intra-task parallelism") {
    Executor executor(8);
    Scheduler scheduler(&executor);

    std::atomic<int> total_sum{0};

    auto parent_task = make_task(
        [&](TaskContext& ctx) {
            std::vector<TaskFuture> level1_futures;

            // Create 3 level-1 children, each spawning their own children
            for (int i = 0; i < 3; ++i) {
                auto level1_child = make_task(
                    [i](TaskContext& ctx_inner) -> int {
                        std::vector<TaskFuture> level2_futures;

                        // Each level-1 child spawns 2 level-2 children
                        for (int j = 0; j < 2; ++j) {
                            auto level2_child = make_task(
                                [i, j]() -> int {
                                    std::this_thread::sleep_for(
                                        std::chrono::milliseconds(5));
                                    return (i + 1) * 10 +
                                           j;  // e.g., 10, 11, 20, 21, 30, 31
                                },
                                "Level2_" + std::to_string(i) + "_" +
                                    std::to_string(j));

                            level2_futures.push_back(
                                ctx_inner.submit_task(level2_child));
                        }

                        // Wait and sum level-2 results
                        int level1_sum = 0;
                        for (auto& f : level2_futures) {
                            level1_sum += f.get<int>();
                        }

                        return level1_sum;
                    },
                    "Level1_" + std::to_string(i));

                level1_futures.push_back(ctx.submit_task(level1_child));
            }

            // Wait and aggregate all level-1 results
            int grand_total = 0;
            for (auto& f : level1_futures) {
                grand_total += f.get<int>();
            }

            total_sum = grand_total;
            return grand_total;
        },
        "RootTaskWithNested");

    scheduler.schedule(parent_task);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should compute: (10+11) + (20+21) + (30+31) = 21 + 41 + 61 = 123
    CHECK(total_sum.load() == 123);
}
// ============================================================================
// Custom Error Handler Tests
// ============================================================================

TEST_CASE("Error Handler - Custom error handling") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<bool> error_handler_called{false};
    std::atomic<bool> handler_done{false};

    scheduler.set_error_handler([&](std::shared_ptr<Task>, std::exception_ptr) {
        error_handler_called = true;
        handler_done = true;
    });

    scheduler.set_error_policy(ErrorPolicy::CONTINUE);

    auto task1 = make_task(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); },
        "Task1");

    auto task2 =
        make_task([]() { throw std::runtime_error("Task2 fails"); }, "Task2");

    auto root = make_task([]() {}, "Root");
    task1->depends_on(root);
    task2->depends_on(root);

    // schedule() blocks until completion with CONTINUE policy
    scheduler.schedule(root);

    // If we reach here, all tasks completed (or failed and continued)
    CHECK(error_handler_called.load());

    // Explicit shutdown to prevent resource leaks
    executor.shutdown();
}

TEST_CASE("Error Handler - Multiple errors with CONTINUE policy") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<int> error_count{0};
    std::atomic<int> expected_errors{3};

    scheduler.set_error_handler(
        [&](std::shared_ptr<Task>, std::exception_ptr) { ++error_count; });

    scheduler.set_error_policy(ErrorPolicy::CONTINUE);

    auto root = make_task([]() {}, "Root");

    // Create 3 failing tasks
    for (int i = 0; i < 3; ++i) {
        auto task = make_task([]() { throw std::runtime_error("Task fails"); },
                              "FailingTask_" + std::to_string(i));
        task->depends_on(root);
    }

    // And 2 successful tasks
    for (int i = 0; i < 2; ++i) {
        auto task = make_task(
            []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            },
            "SuccessTask_" + std::to_string(i));
        task->depends_on(root);
    }

    // schedule() blocks until completion with CONTINUE policy
    scheduler.schedule(root);

    // If we reach here, all tasks completed (or failed and continued)
    CHECK(error_count.load() == 3);
}
// ============================================================================
// Error Policy Tests
// ============================================================================

TEST_CASE("ErrorPolicy - FAIL_FAST stops on first error") {
    auto config =
        PipelineConfigManager().with_executor_threads(4).with_error_policy(
            ErrorPolicy::FAIL_FAST);

    Pipeline pipeline(config);

    // Create a simple DAG: task1 -> task2 -> task3
    // task2 will fail
    std::atomic<int> executed_count{0};

    auto task1 = make_task(
        [&executed_count](int x) -> int {
            executed_count++;
            return x + 1;
        },
        "Task1");

    auto task2 = make_task(
        [&executed_count](int x) -> int {
            executed_count++;
            throw std::runtime_error("Task2 intentional failure");
            return x + 1;
        },
        "Task2");

    auto task3 = make_task(
        [&executed_count](int x) -> int {
            executed_count++;
            return x + 1;
        },
        "Task3");

    task2->depends_on(task1);
    task3->depends_on(task2);

    pipeline.set_source(task1);
    pipeline.set_destination(task3);

    // Execute should throw because task2 fails
    CHECK_THROWS_AS(pipeline.execute(10), PipelineError);

    // Task1 executed, Task2 executed and failed, Task3 should NOT execute
    CHECK(executed_count.load() == 2);
}

TEST_CASE("ErrorPolicy - CONTINUE policy continues other branches") {
    auto config =
        PipelineConfigManager().with_executor_threads(4).with_error_policy(
            ErrorPolicy::CONTINUE);

    Pipeline pipeline(config);

    // Create a diamond DAG:
    //       root
    //      /    (backslash)
    //   task1  task2 (fails)
    //      (backslash)    /
    //      merge
    std::atomic<int> executed_count{0};
    std::atomic<bool> task1_executed{false};
    std::atomic<bool> task2_executed{false};
    std::atomic<bool> merge_executed{false};

    auto root = make_task([](int x) -> int { return x; }, "Root");

    auto task1 = make_task(
        [&](int x) -> int {
            task1_executed = true;
            executed_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return x + 1;
        },
        "Task1");

    auto task2 = make_task(
        [&](int x) -> int {
            task2_executed = true;
            executed_count++;
            throw std::runtime_error("Task2 intentional failure");
            return x + 2;
        },
        "Task2");

    auto merge = make_task(
        [&](int a, int b) -> int {
            merge_executed = true;
            executed_count++;
            return a + b;
        },
        "Merge");

    task1->depends_on(root);
    task2->depends_on(root);
    merge->depends_on(task1);
    merge->depends_on(task2);

    pipeline.set_source(root);
    pipeline.set_destination(merge);

    // Execute should NOT throw - CONTINUE policy
    // But merge should not execute because task2 (one of its parents) failed
    CHECK_NOTHROW(pipeline.execute(10));

    // Root executed, Task1 executed, Task2 executed and failed
    CHECK(task1_executed.load() == true);
    CHECK(task2_executed.load() == true);

    // Merge should NOT execute because task2 failed
    CHECK(merge_executed.load() == false);
}

TEST_CASE("ErrorPolicy - CUSTOM handler is called on error") {
    std::atomic<int> error_handler_calls{0};
    std::string failed_task_name;

    auto config =
        PipelineConfigManager().with_executor_threads(4).with_error_handler(
            [&](std::shared_ptr<Task> task, std::exception_ptr ex) {
                error_handler_calls++;
                failed_task_name = task->get_name();

                // Verify we can access the exception
                try {
                    if (ex) std::rethrow_exception(ex);
                } catch (const std::runtime_error& e) {
                    CHECK(std::string(e.what()).find("intentional") !=
                          std::string::npos);
                }
            });

    Pipeline pipeline(config);

    auto task1 = make_task([](int x) -> int { return x + 1; }, "Task1");

    auto task2 = make_task(
        [](int x) -> int {
            throw std::runtime_error("Task2 intentional failure");
            return x + 1;
        },
        "FailingTask");

    task2->depends_on(task1);

    pipeline.set_source(task1);
    pipeline.set_destination(task2);

    // Execute - error handler should be called
    CHECK_NOTHROW(pipeline.execute(10));

    // Error handler was called exactly once
    CHECK(error_handler_calls.load() == 1);
    CHECK(failed_task_name.find("FailingTask") != std::string::npos);
}

TEST_CASE("ErrorPolicy - CONTINUE skips children of failed tasks") {
    auto config =
        PipelineConfigManager().with_executor_threads(4).with_error_policy(
            ErrorPolicy::CONTINUE);

    Pipeline pipeline(config);

    // Create a chain: task1 -> task2 (fails) -> task3 -> task4
    std::atomic<bool> task1_executed{false};
    std::atomic<bool> task2_executed{false};
    std::atomic<bool> task3_executed{false};
    std::atomic<bool> task4_executed{false};

    auto task1 = make_task(
        [&](int x) -> int {
            task1_executed = true;
            return x + 1;
        },
        "Task1");

    auto task2 = make_task(
        [&](int x) -> int {
            task2_executed = true;
            throw std::runtime_error("Task2 failure");
            return x + 1;
        },
        "Task2");

    auto task3 = make_task(
        [&](int x) -> int {
            task3_executed = true;
            return x + 1;
        },
        "Task3");

    auto task4 = make_task(
        [&](int x) -> int {
            task4_executed = true;
            return x + 1;
        },
        "Task4");

    task2->depends_on(task1);
    task3->depends_on(task2);
    task4->depends_on(task3);

    pipeline.set_source(task1);
    pipeline.set_destination(task4);

    // Execute with CONTINUE policy
    CHECK_NOTHROW(pipeline.execute(10));

    // Task1 executed, Task2 executed and failed
    CHECK(task1_executed.load() == true);
    CHECK(task2_executed.load() == true);

    // Task3 and Task4 should NOT execute (skipped because task2 failed)
    CHECK(task3_executed.load() == false);
    CHECK(task4_executed.load() == false);
}

// ============================================================================
// Multi-threaded Scheduler Tests
// ============================================================================

TEST_CASE("Scheduler - Multiple scheduling threads") {
    auto config =
        PipelineConfigManager().with_executor_threads(8).with_scheduler_threads(
            4);  // 4 scheduling threads

    Pipeline pipeline(config);

    // Create a wide DAG with many independent tasks
    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<Task>> tasks;

    auto root = make_task([](int x) -> int { return x; }, "Root");

    // Create 20 independent tasks
    for (int i = 0; i < 20; ++i) {
        auto task = make_task(
            [&completed, i](int x) -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                completed++;
                return x + i;
            },
            "Task" + std::to_string(i));
        task->depends_on(root);
        tasks.push_back(task);
    }

    pipeline.set_source(root);

    auto start = std::chrono::high_resolution_clock::now();
    CHECK_NOTHROW(pipeline.execute(0));
    auto end = std::chrono::high_resolution_clock::now();

    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // All tasks should complete
    CHECK(completed.load() == 20);

    // With 4 scheduling threads + 8 executor threads, should be much faster
    // than sequential Multi-threading should complete in < 500ms (vs 200ms
    // sequential)
    CHECK(duration.count() < 500);
}

TEST_CASE("Scheduler - Single scheduling thread handles complex DAG") {
    auto config =
        PipelineConfigManager().with_executor_threads(4).with_scheduler_threads(
            1);  // Only 1 scheduling thread

    Pipeline pipeline(config);

    // Create a complex DAG with multiple levels
    std::atomic<int> completed{0};

    auto root = make_task([](int x) -> int { return x; }, "Root");

    std::vector<std::shared_ptr<Task>> level1;
    for (int i = 0; i < 4; ++i) {
        auto task = make_task(
            [&completed](int x) -> int {
                completed++;
                return x + 1;
            },
            "L1_" + std::to_string(i));
        task->depends_on(root);
        level1.push_back(task);
    }

    std::vector<std::shared_ptr<Task>> level2;
    for (int i = 0; i < 4; ++i) {
        auto task = make_task(
            [&completed](int x) -> int {
                completed++;
                return x + 1;
            },
            "L2_" + std::to_string(i));
        task->depends_on(level1[i]);
        level2.push_back(task);
    }

    auto merge = make_task(
        [&completed](int a, int b, int c, int d) -> int {
            completed++;
            return a + b + c + d;
        },
        "Merge");

    merge->depends_on(level2[0]);
    merge->depends_on(level2[1]);
    merge->depends_on(level2[2]);
    merge->depends_on(level2[3]);

    pipeline.set_source(root);
    pipeline.set_destination(merge);

    // Should complete successfully with 1 scheduling thread
    CHECK_NOTHROW(pipeline.execute(0));

    // All tasks completed (4 level1 + 4 level2 + 1 merge = 9)
    CHECK(completed.load() == 9);
}

TEST_CASE(
    "Scheduler - Scheduling threads configured via PipelineConfigManager") {
    // Test that scheduler_threads configuration is properly applied

    auto config1 = PipelineConfigManager().with_scheduler_threads(1);
    Pipeline pipeline1(config1);

    auto config2 = PipelineConfigManager().with_scheduler_threads(3);
    Pipeline pipeline2(config2);

    // Both should construct successfully
    // Actual thread count is internal but configuration should not throw
    CHECK_NOTHROW(pipeline1.validate());
    CHECK_NOTHROW(pipeline2.validate());
}

TEST_CASE(
    "PipelineConfigManager - with_executor_threads sets executor threads") {
    auto config = PipelineConfigManager().with_executor_threads(8);

    CHECK(config.executor_threads == 8);
}

TEST_CASE(
    "PipelineConfigManager - with_scheduler_threads sets scheduler threads") {
    auto config = PipelineConfigManager().with_scheduler_threads(4);

    CHECK(config.scheduler_threads == 4);
}

TEST_CASE("PipelineConfigManager - Fluent API chaining") {
    auto config = PipelineConfigManager()
                      .with_name("TestPipeline")
                      .with_executor_threads(8)
                      .with_scheduler_threads(2)
                      .with_error_policy(ErrorPolicy::CONTINUE)
                      .with_watchdog(true)
                      .with_global_timeout(std::chrono::seconds(60));

    CHECK(config.name == "TestPipeline");
    CHECK(config.executor_threads == 8);
    CHECK(config.scheduler_threads == 2);
    CHECK(config.error_policy == ErrorPolicy::CONTINUE);
    CHECK(config.enable_watchdog == true);
    CHECK(config.global_timeout == std::chrono::seconds(60));
}
