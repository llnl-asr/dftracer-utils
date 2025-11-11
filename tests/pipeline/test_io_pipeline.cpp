#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/io_executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;

// ============================================================================
// IOExecutor Basic Tests
// ============================================================================

TEST_CASE("IOExecutor - Basic construction and destruction") {
    Executor executor(4);

    // Create I/O executor with 2 I/O threads
    executor.create_io_executor(2);

    // Should construct cleanly
    CHECK(executor.get_io_executor() != nullptr);

    // Explicit shutdown
    executor.shutdown();
}

TEST_CASE("IOExecutor - Factory method creates executor") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;

    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    CHECK(io_executor != nullptr);

    io_executor->start();
    io_executor->shutdown();
}

TEST_CASE("IOExecutor - Backend selection on current platform") {
    Executor executor(4);
    executor.create_io_executor(2);

    // IOExecutor should be created with platform-specific backend
    auto* io_exec = executor.get_io_executor();
    CHECK(io_exec != nullptr);

    executor.shutdown();
}

TEST_CASE("IOExecutor - Invalid parameters throw") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;

    // num_io_threads = 0 should throw
    CHECK_THROWS_AS(IOExecutor::create(0, 4, &io_queue), std::invalid_argument);

    // num_workers = 0 should throw
    CHECK_THROWS_AS(IOExecutor::create(2, 0, &io_queue), std::invalid_argument);

    // nullptr queue should throw
    CHECK_THROWS_AS(IOExecutor::create(2, 4, nullptr), std::invalid_argument);
}

TEST_CASE("IOExecutor - Start and shutdown lifecycle") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    // Start I/O threads
    io_executor->start();

    // Give threads time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Shutdown
    io_executor->shutdown();

    // Should be able to shutdown again without issue
    io_executor->shutdown();
}

TEST_CASE("IOExecutor - Multiple I/O threads") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;

    // Create with 4 I/O threads
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    CHECK(io_executor != nullptr);

    io_executor->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    io_executor->shutdown();
}

// ============================================================================
// IOExecutor Integration with Executor Tests
// ============================================================================

TEST_CASE("Integration - Executor with IOExecutor") {
    Executor executor(4);

    // Create I/O executor before starting
    executor.create_io_executor(2);

    CHECK(executor.get_io_executor() != nullptr);

    // Shutdown should clean up both executor and io_executor
    executor.shutdown();

    // After shutdown, io_executor should be reset
    CHECK(executor.get_io_executor() == nullptr);
}

TEST_CASE("Integration - Cannot create IOExecutor while running") {
    Executor executor(4);
    Scheduler scheduler(&executor);

    std::atomic<bool> task_started{false};

    auto task = make_task(
        [&]() {
            task_started = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        },
        "LongTask");

    // Schedule task in background
    std::thread scheduler_thread([&]() {
        try {
            scheduler.schedule(task);
        } catch (...) {
        }
    });

    // Wait for task to start
    while (!task_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Try to create I/O executor while running - should throw
    CHECK_THROWS_AS(executor.create_io_executor(2), std::runtime_error);

    scheduler.request_shutdown();
    scheduler_thread.join();
    executor.shutdown();
}

TEST_CASE("Integration - IOExecutor with different thread counts") {
    {
        Executor executor(2);
        executor.create_io_executor(1);  // 1 I/O thread
        CHECK(executor.get_io_executor() != nullptr);
        executor.shutdown();
    }

    {
        Executor executor(8);
        executor.create_io_executor(4);  // 4 I/O threads
        CHECK(executor.get_io_executor() != nullptr);
        executor.shutdown();
    }
}

// ============================================================================
// Backend Selection Tests
// ============================================================================

TEST_CASE("Backend - Platform-specific backend is selected") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    // IOExecutor should have created a backend
    // (We can't directly test which backend, but we can verify it works)
    CHECK(io_executor != nullptr);

    io_executor->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    io_executor->shutdown();
}

TEST_CASE("Backend - IOBackend factory returns non-null") {
    auto backend = IOBackend::create();

    CHECK(backend != nullptr);
    CHECK(backend->name() != nullptr);

#ifdef __linux__
    // On Linux, should be io_uring or thread_pool
    std::string name = backend->name();
    CHECK((name == "io_uring" || name == "thread_pool"));
#elif defined(__APPLE__)
    // On macOS, should be kqueue
    CHECK(std::string(backend->name()) == "kqueue");
#else
    // On other platforms, should be thread_pool
    CHECK(std::string(backend->name()) == "thread_pool");
#endif
}

TEST_CASE("Backend - Platform availability check") {
    bool available = IOBackend::is_available_on_platform();

    // Should always be true (thread pool is always available as fallback)
    CHECK(available == true);
}

// ============================================================================
// Fast Path Queue Tests
// ============================================================================

TEST_CASE("FastPath - try_pop_fast_path with invalid worker") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    TaskItem item;

    // Invalid worker ID should return false
    CHECK(io_executor->try_pop_fast_path(100, item) == false);
}

TEST_CASE("FastPath - try_pop_fast_path with empty queue") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    TaskItem item;

    // Valid worker ID but empty queue should return false
    CHECK(io_executor->try_pop_fast_path(0, item) == false);
    CHECK(io_executor->try_pop_fast_path(3, item) == false);
}

// ============================================================================
// Queue Type Tests
// ============================================================================

TEST_CASE("QueueTypes - Executor uses separate queues") {
    Executor executor(4);

    // Create I/O executor
    executor.create_io_executor(2);

    // Executor should have:
    // - shared_queue_ (ConcurrentQueue for CPU tasks)
    // - io_slow_path_queue_ (BlockingConcurrentQueue for I/O tasks)

    CHECK(executor.get_io_executor() != nullptr);

    executor.shutdown();
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_CASE("Stress - Multiple IOExecutor create/destroy cycles") {
    for (int i = 0; i < 5; ++i) {
        Executor executor(4);
        executor.create_io_executor(2);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        executor.shutdown();
    }
}

TEST_CASE("Stress - IOExecutor with high thread count") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;

    // Create with many I/O threads
    auto io_executor = IOExecutor::create(8, 16, &io_queue);

    io_executor->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    io_executor->shutdown();
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("Error - Double create_io_executor throws") {
    Executor executor(4);

    executor.create_io_executor(2);

    // Second create should throw
    CHECK_THROWS(executor.create_io_executor(2));

    executor.shutdown();
}

TEST_CASE("Error - Shutdown handles missing IOExecutor gracefully") {
    Executor executor(4);

    // Shutdown without creating IOExecutor should work
    CHECK_NOTHROW(executor.shutdown());
}

// ============================================================================
// Timing Tests
// ============================================================================

TEST_CASE("Timing - IOExecutor starts within reasonable time") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    auto start = std::chrono::steady_clock::now();
    io_executor->start();
    auto end = std::chrono::steady_clock::now();

    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Starting should be fast (< 100ms)
    CHECK(duration.count() < 100);

    io_executor->shutdown();
}

TEST_CASE("Timing - IOExecutor shutdown within reasonable time") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    io_executor->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto start = std::chrono::steady_clock::now();
    io_executor->shutdown();
    auto end = std::chrono::steady_clock::now();

    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Shutdown should be reasonably fast (< 500ms)
    CHECK(duration.count() < 500);
}

// ============================================================================
// Actual I/O Operation Tests
// ============================================================================

// Dummy coroutine handle for testing
struct TestCoroutine {
    struct promise_type {
        TestCoroutine get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

TEST_CASE("IO Operations - Submit and execute simple I/O") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    std::atomic<int> counter{0};
    std::atomic<bool> io_executed{false};

    // Submit an I/O operation
    auto io_func = [&]() {
        // Simulate I/O work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        counter.fetch_add(1);
        io_executed.store(true);
    };

    // Submit without coroutine continuation (just execute the I/O)
    uint64_t request_id =
        io_executor->submit_io_operation(io_func, std::coroutine_handle<>{}, 0);

    CHECK(request_id > 0);

    // Wait for I/O to complete
    auto start = std::chrono::steady_clock::now();
    while (!io_executed.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(io_executed.load() == true);
    CHECK(counter.load() == 1);
    CHECK(io_executor->get_completed_io_ops() >= 1);

    io_executor->shutdown();
}

TEST_CASE("IO Operations - Multiple concurrent I/O operations") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    io_executor->start();

    std::atomic<int> counter{0};
    constexpr int NUM_OPS = 20;

    // Submit multiple I/O operations
    for (int i = 0; i < NUM_OPS; ++i) {
        auto io_func = [&counter]() {
            // Simulate I/O work
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            counter.fetch_add(1);
        };

        io_executor->submit_io_operation(io_func, std::coroutine_handle<>{},
                                         i % 8);
    }

    // Wait for all I/O operations to complete
    auto start = std::chrono::steady_clock::now();
    while (counter.load() < NUM_OPS &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(counter.load() == NUM_OPS);
    CHECK(io_executor->get_completed_io_ops() >= NUM_OPS);
    CHECK(io_executor->get_pending_io_ops() == 0);

    io_executor->shutdown();
}

TEST_CASE("IO Operations - I/O with different worker targets") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    std::atomic<int> worker_0_count{0};
    std::atomic<int> worker_1_count{0};
    std::atomic<int> worker_2_count{0};
    std::atomic<int> worker_3_count{0};

    // Submit I/O operations targeting different workers
    auto submit_for_worker = [&](std::size_t worker_id,
                                 std::atomic<int>& count) {
        for (int i = 0; i < 5; ++i) {
            auto io_func = [&count]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                count.fetch_add(1);
            };
            io_executor->submit_io_operation(io_func, std::coroutine_handle<>{},
                                             worker_id);
        }
    };

    submit_for_worker(0, worker_0_count);
    submit_for_worker(1, worker_1_count);
    submit_for_worker(2, worker_2_count);
    submit_for_worker(3, worker_3_count);

    // Wait for all operations to complete
    auto start = std::chrono::steady_clock::now();
    while ((worker_0_count.load() + worker_1_count.load() +
            worker_2_count.load() + worker_3_count.load()) < 20 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(worker_0_count.load() == 5);
    CHECK(worker_1_count.load() == 5);
    CHECK(worker_2_count.load() == 5);
    CHECK(worker_3_count.load() == 5);

    io_executor->shutdown();
}

TEST_CASE("IO Operations - Fast path queue routing") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    std::atomic<int> completed{0};
    constexpr int NUM_OPS = 50;

    // Submit many operations to test fast path routing
    for (int i = 0; i < NUM_OPS; ++i) {
        auto io_func = [&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        };
        io_executor->submit_io_operation(io_func, std::coroutine_handle<>{},
                                         i % 4);
    }

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < NUM_OPS &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(completed.load() == NUM_OPS);
    CHECK(io_executor->get_completed_io_ops() >= NUM_OPS);

    // Check that we achieved some fast path hits
    // (exact ratio depends on timing, but should be > 0)
    double hit_rate = io_executor->get_fast_path_hit_rate();
    // We can't guarantee a specific hit rate, but it should be computed
    // correctly
    CHECK(hit_rate >= 0.0);
    CHECK(hit_rate <= 100.0);

    io_executor->shutdown();
}

TEST_CASE("IO Operations - Metrics tracking") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    // Initial state
    CHECK(io_executor->get_pending_io_ops() == 0);
    CHECK(io_executor->get_completed_io_ops() == 0);

    std::atomic<int> completed{0};
    constexpr int NUM_OPS = 10;

    // Submit operations
    for (int i = 0; i < NUM_OPS; ++i) {
        auto io_func = [&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            completed.fetch_add(1);
        };
        io_executor->submit_io_operation(io_func, std::coroutine_handle<>{}, 0);
    }

    // Pending count should increase
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(io_executor->get_pending_io_ops() > 0);

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < NUM_OPS &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // After completion
    CHECK(io_executor->get_completed_io_ops() >= NUM_OPS);
    CHECK(io_executor->get_pending_io_ops() == 0);

    io_executor->shutdown();
}

TEST_CASE("IO Operations - Backend name verification") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    // Backend should be initialized
    const char* backend_name = io_executor->get_backend_name();
    CHECK(backend_name != nullptr);
    CHECK(std::string(backend_name) != "none");

#ifdef __linux__
    // On Linux, should be io_uring or thread_pool
    std::string name = backend_name;
    CHECK((name == "io_uring" || name == "thread_pool"));
#elif defined(__APPLE__)
    // On macOS, should be kqueue
    CHECK(std::string(backend_name) == "kqueue");
#else
    // On other platforms, should be thread_pool
    CHECK(std::string(backend_name) == "thread_pool");
#endif

    io_executor->start();
    io_executor->shutdown();
}

TEST_CASE("IO Operations - Error handling for not running") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    // Don't start the executor
    CHECK(io_executor->is_running() == false);

    // Submitting I/O should throw
    auto io_func = []() { /* noop */ };
    CHECK_THROWS_AS(
        io_executor->submit_io_operation(io_func, std::coroutine_handle<>{}, 0),
        std::runtime_error);
}

TEST_CASE("IO Operations - Stress test with many operations") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    io_executor->start();

    std::atomic<int> completed{0};
    constexpr int NUM_OPS = 200;

    // Submit many I/O operations rapidly
    for (int i = 0; i < NUM_OPS; ++i) {
        auto io_func = [&completed]() {
            // Very short I/O operation
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            completed.fetch_add(1);
        };
        io_executor->submit_io_operation(io_func, std::coroutine_handle<>{},
                                         i % 8);
    }

    // Wait for all to complete
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < NUM_OPS &&
           std::chrono::steady_clock::now() - start <
               std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(completed.load() == NUM_OPS);
    CHECK(io_executor->get_completed_io_ops() >= NUM_OPS);
    CHECK(io_executor->get_pending_io_ops() == 0);

    io_executor->shutdown();
}

// ============================================================================
// Real File I/O Tests
// ============================================================================

TEST_CASE("Real IO - Write and read file") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    const auto test_file =
        (fs::temp_directory_path() / "test_io_executor_write_read.txt")
            .string();
    const std::string test_data = "Hello from IOExecutor!";
    std::atomic<bool> write_done{false};
    std::atomic<bool> read_done{false};
    std::string read_data;

    // Write operation
    auto write_func = [&]() {
        FILE* f = fopen(test_file.c_str(), "w");
        if (f) {
            fwrite(test_data.c_str(), 1, test_data.size(), f);
            fclose(f);
            write_done.store(true);
        }
    };

    io_executor->submit_io_operation(write_func, std::coroutine_handle<>{}, 0);

    // Wait for write to complete
    auto start = std::chrono::steady_clock::now();
    while (!write_done.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(write_done.load() == true);

    // Read operation
    auto read_func = [&]() {
        FILE* f = fopen(test_file.c_str(), "r");
        if (f) {
            char buffer[256];
            size_t n = fread(buffer, 1, sizeof(buffer), f);
            read_data = std::string(buffer, n);
            fclose(f);
            read_done.store(true);
        }
    };

    io_executor->submit_io_operation(read_func, std::coroutine_handle<>{}, 1);

    // Wait for read to complete
    start = std::chrono::steady_clock::now();
    while (!read_done.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(read_done.load() == true);
    CHECK(read_data == test_data);

    // Cleanup
    std::remove(test_file.c_str());

    io_executor->shutdown();
}

TEST_CASE("Real IO - Multiple file writes") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    io_executor->start();

    constexpr int NUM_FILES = 10;
    std::atomic<int> completed{0};
    std::vector<std::string> file_paths;

    // Create multiple write operations
    for (int i = 0; i < NUM_FILES; ++i) {
        auto file_path =
            (fs::temp_directory_path() /
             ("test_io_executor_file_" + std::to_string(i) + ".txt"))
                .string();
        file_paths.push_back(file_path);

        auto write_func = [file_path, i, &completed]() {
            FILE* f = fopen(file_path.c_str(), "w");
            if (f) {
                std::string content =
                    "File " + std::to_string(i) + " content\n";
                fwrite(content.c_str(), 1, content.size(), f);
                fclose(f);
                completed.fetch_add(1);
            }
        };

        io_executor->submit_io_operation(write_func, std::coroutine_handle<>{},
                                         i % 8);
    }

    // Wait for all writes to complete
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < NUM_FILES &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(completed.load() == NUM_FILES);

    // Verify all files exist
    for (const auto& path : file_paths) {
        FILE* f = fopen(path.c_str(), "r");
        CHECK(f != nullptr);
        if (f) {
            fclose(f);
        }
    }

    // Cleanup
    for (const auto& path : file_paths) {
        std::remove(path.c_str());
    }

    io_executor->shutdown();
}

TEST_CASE("Real IO - Concurrent reads and writes") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(4, 8, &io_queue);

    io_executor->start();

    constexpr int NUM_OPS = 20;
    std::atomic<int> write_count{0};
    std::atomic<int> read_count{0};
    std::vector<std::string> file_paths;

    // Submit interleaved write and read operations
    for (int i = 0; i < NUM_OPS / 2; ++i) {
        auto file_path = (fs::temp_directory_path() /
                          ("test_io_concurrent_" + std::to_string(i) + ".txt"))
                             .string();
        file_paths.push_back(file_path);

        // Write operation
        auto write_func = [file_path, i, &write_count]() {
            FILE* f = fopen(file_path.c_str(), "w");
            if (f) {
                std::string content = "Data " + std::to_string(i * 100);
                fwrite(content.c_str(), 1, content.size(), f);
                fclose(f);
                write_count.fetch_add(1);
            }
        };

        io_executor->submit_io_operation(write_func, std::coroutine_handle<>{},
                                         i % 8);

        // Short delay to ensure write happens first
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Read operation
        auto read_func = [file_path, &read_count]() {
            FILE* f = fopen(file_path.c_str(), "r");
            if (f) {
                char buffer[256];
                fread(buffer, 1, sizeof(buffer), f);
                fclose(f);
                read_count.fetch_add(1);
            }
        };

        io_executor->submit_io_operation(read_func, std::coroutine_handle<>{},
                                         (i + 1) % 8);
    }

    // Wait for all operations to complete
    auto start = std::chrono::steady_clock::now();
    while ((write_count.load() + read_count.load()) < NUM_OPS &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(write_count.load() == NUM_OPS / 2);
    CHECK(read_count.load() == NUM_OPS / 2);

    // Cleanup
    for (const auto& path : file_paths) {
        std::remove(path.c_str());
    }

    io_executor->shutdown();
}

TEST_CASE("Real IO - Large file write and read") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    const auto test_file =
        (fs::temp_directory_path() / "test_io_large_file.bin").string();
    constexpr size_t FILE_SIZE = 1024 * 1024;  // 1 MB
    std::vector<char> write_data(FILE_SIZE);

    // Fill with pattern
    for (size_t i = 0; i < FILE_SIZE; ++i) {
        write_data[i] = static_cast<char>(i % 256);
    }

    std::atomic<bool> write_done{false};
    std::atomic<bool> read_done{false};
    std::vector<char> read_data;

    // Write large file
    auto write_func = [&]() {
        FILE* f = fopen(test_file.c_str(), "wb");
        if (f) {
            fwrite(write_data.data(), 1, write_data.size(), f);
            fclose(f);
            write_done.store(true);
        }
    };

    io_executor->submit_io_operation(write_func, std::coroutine_handle<>{}, 0);

    // Wait for write
    auto start = std::chrono::steady_clock::now();
    while (!write_done.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(write_done.load() == true);

    // Read large file
    auto read_func = [&]() {
        FILE* f = fopen(test_file.c_str(), "rb");
        if (f) {
            read_data.resize(FILE_SIZE);
            size_t n = fread(read_data.data(), 1, FILE_SIZE, f);
            read_data.resize(n);
            fclose(f);
            read_done.store(true);
        }
    };

    io_executor->submit_io_operation(read_func, std::coroutine_handle<>{}, 1);

    // Wait for read
    start = std::chrono::steady_clock::now();
    while (!read_done.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(read_done.load() == true);
    CHECK(read_data.size() == FILE_SIZE);
    CHECK(read_data == write_data);

    // Cleanup
    std::remove(test_file.c_str());

    io_executor->shutdown();
}

TEST_CASE("Real IO - File stat operations") {
    moodycamel::BlockingConcurrentQueue<TaskItem> io_queue;
    auto io_executor = IOExecutor::create(2, 4, &io_queue);

    io_executor->start();

    const auto test_file =
        (fs::temp_directory_path() / "test_io_stat.txt").string();
    const std::string test_content = "Test content for stat";

    std::atomic<bool> write_done{false};
    std::atomic<bool> stat_done{false};
    std::atomic<size_t> file_size{0};

    // Write file
    auto write_func = [&]() {
        FILE* f = fopen(test_file.c_str(), "w");
        if (f) {
            fwrite(test_content.c_str(), 1, test_content.size(), f);
            fclose(f);
            write_done.store(true);
        }
    };

    io_executor->submit_io_operation(write_func, std::coroutine_handle<>{}, 0);

    // Wait for write
    auto start = std::chrono::steady_clock::now();
    while (!write_done.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(write_done.load() == true);

    // Stat file
    auto stat_func = [&]() {
        FILE* f = fopen(test_file.c_str(), "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            file_size.store(static_cast<size_t>(size));
            fclose(f);
            stat_done.store(true);
        }
    };

    io_executor->submit_io_operation(stat_func, std::coroutine_handle<>{}, 1);

    // Wait for stat
    start = std::chrono::steady_clock::now();
    while (!stat_done.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(stat_done.load() == true);
    CHECK(file_size.load() == test_content.size());

    // Cleanup
    std::remove(test_file.c_str());

    io_executor->shutdown();
}
