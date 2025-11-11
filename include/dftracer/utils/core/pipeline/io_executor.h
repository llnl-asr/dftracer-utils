#ifndef DFTRACER_UTILS_CORE_PIPELINE_IO_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_IO_EXECUTOR_H

#include <blockingconcurrentqueue.h>
#include <readerwriterqueue.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "dftracer/utils/core/pipeline/io_backend.h"
#include "dftracer/utils/core/pipeline/task_item.h"

namespace dftracer::utils {

/**
 * IOExecutor - Dedicated I/O execution engine
 *
 * IMPORTANT: This is an internal component. Users never instantiate this.
 * Pipeline creates and attaches it to Executor based on config.
 *
 * Architecture:
 * - Dedicated I/O thread pool (2-4 threads typically)
 * - Fast path: SPSC queues (I/O thread -> specific Worker)
 * - Slow path: Shared MPMC queue (overflow + general submissions)
 * - Pluggable backends (io_uring on Linux, kqueue on macOS)
 * - Zero overhead when disabled
 *
 * Design:
 * I/O threads monitor async I/O completions and route completed operations
 * back to worker threads via lock-free queues for minimal latency.
 *
 * Queue Priority:
 * 1. Fast path SPSC - I/O completion → target worker
 * 2. Slow path MPMC - Overflow or external submissions
 */
class IOExecutor {
   public:
    /**
     * I/O operation request (submitted by coroutines)
     */
    struct IORequest {
        std::function<void()> io_func;         // I/O operation to execute
        std::coroutine_handle<> continuation;  // Coroutine to resume
        std::size_t target_worker_id;  // Preferred worker for resumption
        uint64_t request_id;           // Unique request identifier

        IORequest() : target_worker_id(0), request_id(0) {}

        IORequest(std::function<void()> func, std::coroutine_handle<> cont,
                  std::size_t worker_id = 0)
            : io_func(std::move(func)),
              continuation(cont),
              target_worker_id(worker_id),
              request_id(0) {}
    };

   private:
    // I/O thread pool
    std::vector<std::thread> io_threads_;
    std::size_t num_io_threads_;
    std::atomic<bool> running_{false};

    // Number of worker threads (for SPSC matrix sizing)
    std::size_t num_workers_;

    // Fast path: SPSC queues (I/O thread → Worker)
    // Matrix: fast_path_queues_[io_thread_id][worker_id]
    struct FastPathQueue {
        moodycamel::ReaderWriterQueue<TaskItem> queue;
        FastPathQueue() : queue(1024) {}  // 1K items per SPSC queue
    };
    std::vector<std::vector<std::unique_ptr<FastPathQueue>>> fast_path_queues_;

    // Slow path: Shared MPMC queue (reference to Executor's shared queue)
    moodycamel::BlockingConcurrentQueue<TaskItem>* slow_path_queue_;

    // I/O backend (platform-specific: io_uring, kqueue, or thread pool)
    std::unique_ptr<IOBackend> io_backend_;

    // Pending I/O requests queue
    moodycamel::BlockingConcurrentQueue<IORequest> io_request_queue_;

    // Metrics
    std::atomic<std::size_t> pending_io_ops_{0};
    std::atomic<std::size_t> completed_io_ops_{0};
    std::atomic<std::size_t> fast_path_enqueues_{0};
    std::atomic<std::size_t> slow_path_fallbacks_{0};

    // Round-robin counter for load balancing
    std::atomic<std::size_t> next_target_worker_{0};

    // Request ID counter
    std::atomic<uint64_t> next_request_id_{1};

   public:
    /**
     * Factory method to create IOExecutor
     *
     * @param num_io_threads Number of I/O threads (typically 2-4)
     * @param num_workers Number of worker threads (for SPSC matrix sizing)
     * @param slow_path_queue Shared MPMC queue from Executor
     * @return Unique pointer to IOExecutor instance
     */
    static std::unique_ptr<IOExecutor> create(
        std::size_t num_io_threads, std::size_t num_workers,
        moodycamel::BlockingConcurrentQueue<TaskItem>* slow_path_queue);

    ~IOExecutor();

   private:
    /**
     * Constructor (private - use create() factory method)
     *
     * @param num_io_threads Number of I/O threads (typically 2-4)
     * @param num_workers Number of worker threads (for SPSC matrix sizing)
     * @param slow_path_queue Shared MPMC queue from Executor
     */
    IOExecutor(std::size_t num_io_threads, std::size_t num_workers,
               moodycamel::BlockingConcurrentQueue<TaskItem>* slow_path_queue);

   public:
    // Non-copyable, non-movable
    IOExecutor(const IOExecutor&) = delete;
    IOExecutor& operator=(const IOExecutor&) = delete;
    IOExecutor(IOExecutor&&) = delete;
    IOExecutor& operator=(IOExecutor&&) = delete;

    /**
     * Start I/O executor (spawn I/O threads)
     */
    void start();

    /**
     * Shutdown I/O executor gracefully
     * Waits for pending I/O operations to complete
     */
    void shutdown();

    /**
     * Check if I/O executor is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * Submit I/O operation for async execution
     *
     * Called by worker threads when coroutines execute spawn_io().
     * The I/O operation will be executed on an I/O thread, and the
     * coroutine will be resumed on a worker thread when complete.
     *
     * @param io_func I/O operation to execute
     * @param continuation Coroutine to resume after I/O completes
     * @param preferred_worker_id Preferred worker for resumption (optional)
     * @return Request ID
     */
    uint64_t submit_io_operation(std::function<void()> io_func,
                                 std::coroutine_handle<> continuation,
                                 std::size_t preferred_worker_id = SIZE_MAX);

    /**
     * Check if worker has pending fast-path I/O completions
     *
     * Called by worker thread during work-stealing priority checks.
     *
     * @param worker_id Worker ID to check
     * @return true if fast-path queue has items
     */
    bool has_fast_path_work(std::size_t worker_id) const;

    /**
     * Try to pop from fast-path queue (called by worker thread)
     *
     * Worker threads check their fast-path queues first for minimal latency.
     * This is the highest priority work queue (PRIORITY 1).
     *
     * @param worker_id Worker ID
     * @param item Output task item
     * @return true if task was retrieved
     */
    bool try_pop_fast_path(std::size_t worker_id, TaskItem& item);

    // ========================================================================
    // Metrics and monitoring
    // ========================================================================

    /**
     * Get number of pending I/O operations
     */
    std::size_t get_pending_io_ops() const { return pending_io_ops_.load(); }

    /**
     * Get number of completed I/O operations
     */
    std::size_t get_completed_io_ops() const {
        return completed_io_ops_.load();
    }

    /**
     * Get fast-path hit rate (percentage)
     * High hit rate (>95%) means low contention, good performance
     */
    double get_fast_path_hit_rate() const {
        std::size_t total =
            fast_path_enqueues_.load() + slow_path_fallbacks_.load();
        if (total == 0) return 0.0;
        return (100.0 * static_cast<double>(fast_path_enqueues_.load())) /
               static_cast<double>(total);
    }

    /**
     * Get number of I/O threads
     */
    std::size_t get_num_io_threads() const { return num_io_threads_; }

    /**
     * Get I/O backend name
     */
    const char* get_backend_name() const {
        return io_backend_ ? io_backend_->name() : "none";
    }

   private:
    /**
     * I/O thread main loop
     *
     * Each I/O thread:
     * 1. Dequeues I/O requests
     * 2. Executes I/O operations (blocking)
     * 3. Routes completions to target worker via SPSC (fast path)
     * 4. Falls back to MPMC (slow path) if SPSC full
     *
     * @param io_thread_id ID of this I/O thread
     */
    void io_thread_loop(std::size_t io_thread_id);

    /**
     * Execute single I/O request and route completion
     *
     * @param io_thread_id ID of executing I/O thread
     * @param request I/O request to execute
     */
    void execute_io_request(std::size_t io_thread_id, const IORequest& request);

    /**
     * Route completed I/O operation to worker thread
     *
     * Try fast path (SPSC) first, fall back to slow path (MPMC).
     *
     * @param io_thread_id ID of I/O thread routing the completion
     * @param target_worker_id Target worker for resumption
     * @param item Task item (contains coroutine handle)
     */
    void route_completion_to_worker(std::size_t io_thread_id,
                                    std::size_t target_worker_id,
                                    const TaskItem& item);

    /**
     * Select target worker for I/O completion (round-robin)
     */
    std::size_t select_target_worker();
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_IO_EXECUTOR_H
