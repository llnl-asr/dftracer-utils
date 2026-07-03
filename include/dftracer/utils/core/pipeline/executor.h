#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTOR_H

#include <concurrentqueue.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/common/timer_service.h>
#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io_backend.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils {

class Task;
class CoroScope;
class Scheduler;

struct ExecutorConfig {
    std::size_t num_threads = 0;   // 0 = hardware_concurrency
    std::chrono::seconds idle_timeout{5};
    std::chrono::seconds deadlock_timeout{10};
    std::size_t io_pool_size = 0;  // 0 = hardware_concurrency
    io::IoBackendType io_backend_type = io::IoBackendType::AUTO;
    unsigned io_batch_threshold = 16;
};

/**
 * Task information for progress tracking
 */
struct TaskInfo {
    TaskIndex task_id;
    TaskIndex parent_task_id;  // -1 for root tasks
    std::string name;
    std::size_t worker_id;     // Which worker is executing

    enum State {
        QUEUED,                // In queue (shared or local)
        RUNNING,               // Currently executing
        WAITING,               // Waiting for child tasks
        COMPLETED,             // Successfully finished
        FAILED                 // Failed with error
    } state;

    std::chrono::steady_clock::time_point queued_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;

    // Child tracking
    std::vector<TaskIndex> child_task_ids;
    std::atomic<std::size_t> completed_children{0};

    // Error info
    std::string error_message;

    // Queue location
    enum Location { SHARED_QUEUE, LOCAL_QUEUE, EXECUTING, DONE } location;
};

/**
 * Task progress information
 */
struct TaskProgress {
    TaskIndex task_id;
    std::string name;
    std::string state;  // "queued", "running", "waiting", "completed", "failed"

    // Timing
    double queued_duration_ms;
    double execution_duration_ms;

    // Progress
    std::size_t total_subtasks;
    std::size_t completed_subtasks;
    double progress_percentage;  // 0-100

    // Location
    // "shared_queue", "worker_2_local", "executing_on_worker_3"
    std::string location;

    // Children
    std::vector<TaskProgress> children;  // Recursive structure!
};

/**
 * Executor progress report
 */
struct ExecutorProgress {
    // Overall stats
    std::size_t total_tasks_submitted;
    std::size_t tasks_queued;
    std::size_t tasks_running;
    std::size_t tasks_completed;
    std::size_t tasks_failed;

    // Queue depths
    std::vector<std::size_t> worker_queue_depths;

    // Task tree (root tasks with their children)
    std::vector<TaskProgress> root_tasks;

    // Worker states
    struct WorkerStatus {
        std::size_t worker_id;
        bool is_idle;
        std::optional<TaskIndex> current_task_id;
        std::string current_task_name;
        std::size_t local_queue_depth;
    };
    std::vector<WorkerStatus> workers;

    // Errors
    // task_id, error_msg
    std::vector<std::pair<TaskIndex, std::string>> recent_errors;
};

/**
 * Executor - Executes tasks from queue using worker thread pool
 *
 * Features:
 * - Large thread pool for CPU/IO-bound work
 * - Pulls tasks from queue
 * - Executes task functions
 * - Notifies scheduler on completion via callback
 *
 * Thread pool size: N threads (default: hardware_concurrency)
 */
class Executor {
   public:
    using CompletionCallback = std::function<void(std::shared_ptr<Task>)>;

   private:
    // Worker context for per-thread state.
    // Aligned to avoid false sharing between adjacent workers.
    struct alignas(DFTRACER_OPTIMAL_ALIGNMENT) WorkerContext {
        std::size_t worker_id;

        // Health monitoring for watchdog
        std::atomic<bool> is_idle{false};
        std::atomic<uint64_t> tasks_executed{0};
        std::chrono::steady_clock::time_point last_activity;

        // Current task info (for debugging/watchdog)
        std::atomic<TaskIndex> current_task_id{-1};
        std::string current_task_name;
        std::mutex task_name_mutex;

        // Worker thread
        std::thread thread;

        explicit WorkerContext(std::size_t id) : worker_id(id) {}
    };

    // Per-worker contexts
    std::vector<std::unique_ptr<WorkerContext>> workers_;
    std::atomic<std::size_t> next_worker_{0};  // For round-robin submission

    std::atomic<bool> running_{false};
    std::size_t num_threads_;

    CompletionCallback completion_callback_;

    // Reference to scheduler for dynamic task context
    Scheduler* scheduler_{nullptr};

    // Global tracking, padded to avoid false sharing between counters
    // and with work_signal_ below.
    alignas(DFTRACER_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> tasks_completed_{0};
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) std::atomic<std::size_t> tasks_started_{
        0};
    alignas(DFTRACER_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> total_tasks_submitted_{0};

    std::atomic<std::int64_t> last_activity_ns_;

    // Shutdown coordination
    std::atomic<bool> shutdown_requested_{false};

    // Responsiveness timeout thresholds
    std::chrono::seconds idle_timeout_;
    std::chrono::seconds deadlock_timeout_;
    // Timer service for timeout operations
    TimerService timer_service_;

    // Task registry for progress tracking
    std::unordered_map<TaskIndex, TaskInfo> task_registry_;
    mutable std::shared_mutex registry_mutex_;

    // Counter for tracked-coro IDs (negative to avoid collision with DAG IDs).
    std::atomic<TaskIndex> next_coro_task_id_{-1000000};

    // Run queue entry. -1 means untracked.
    struct RunQueueEntry {
        std::coroutine_handle<> handle{};
        TaskIndex task_id{-1};
        long long monitor_id{-1};  // coroutine monitor id, -1 if untracked
    };

    moodycamel::ConcurrentQueue<RunQueueEntry> run_queue_;
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) std::atomic<std::uint64_t> work_signal_{
        0};

    // Deferred destruction queue for released Coro handles.
    // FinalAwaiter pushes here; worker loop drains periodically.
    moodycamel::ConcurrentQueue<std::coroutine_handle<>> destroy_queue_;

    // I/O backend (owned by executor, created by factory)
    std::unique_ptr<io::IoBackend> io_backend_;

    // Configuration (stored from ExecutorConfig)
    std::size_t io_pool_size_ = 0;
    io::IoBackendType io_backend_type_ = io::IoBackendType::AUTO;
    unsigned io_batch_threshold_ = 16;

   public:
    /**
     * Constructor
     */
    explicit Executor(const ExecutorConfig& config = {});

    ~Executor();

    // Prevent copying
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Prevent moving (threads are not movable once started)
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    /**
     * Start the executor (spawn worker threads)
     */
    void start();

    /**
     * Shutdown the executor gracefully
     */
    void shutdown();

    /**
     * Reset the executor (prepare for new execution)
     */
    void reset();

    /**
     * Set completion callback (called when task finishes)
     */
    void set_completion_callback(CompletionCallback callback);

    /**
     * Set scheduler reference (for CoroScope)
     */
    void set_scheduler(Scheduler* scheduler) { scheduler_ = scheduler; }

    /**
     * Get timer service for timeout operations
     */
    TimerService& get_timer_service() { return timer_service_; }

    /**
     * Check if executor is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * Get number of worker threads
     */
    std::size_t get_num_threads() const { return num_threads_; }

    std::size_t get_io_pool_size() const { return io_pool_size_; }

    /**
     * Check if an I/O backend is available
     */
    bool has_io_backend() const noexcept { return io_backend_ != nullptr; }

    /**
     * Get the I/O backend (must check has_io_backend() first)
     */
    io::IoBackend& io_backend() { return *io_backend_; }
    const io::IoBackend& io_backend() const { return *io_backend_; }

    /**
     * Get the executor running on the current worker thread (nullptr
     * if the calling thread is not a worker).  Thread-local.
     */
    static Executor* current() noexcept;

    /**
     * Set the current-thread executor TLS, returning the old value.
     * Used by CoroTask::get() to suppress async I/O submission
     * when driving a coroutine synchronously.
     */
    static Executor* set_current(Executor* e) noexcept;
    /**
     * Request graceful shutdown
     * Stops accepting new tasks and waits for current tasks to complete
     */
    void request_shutdown();

    /**
     * Check if shutdown was requested
     */
    bool is_shutdown_requested() const { return shutdown_requested_.load(); }

    /**
     * Check if executor is responsive (making progress)
     *
     * Used by watchdog to detect if executor is hung.
     * Returns false if executor appears to be stuck or unresponsive.
     */
    bool is_responsive() const;

    /**
     * Get full progress report
     */
    ExecutorProgress get_progress() const;

    /**
     * Schedule a coroutine handle to be resumed on the executor's thread pool
     * This is a lightweight operation that submits the resumption as work
     * @param handle The coroutine handle to resume
     *
     * This is useful for when_all and other coroutine combinators that need
     * to resume coroutines from completion callbacks without directly calling
     * .resume()
     */
    void schedule_coroutine_resumption(std::coroutine_handle<> handle);

    /**
     * Enqueue a coroutine handle for execution on the thread pool.
     * This is the primary submission method for goroutines -- all
     * lightweight work funnels through here.
     * Cost: ~20ns (lock-free queue push + atomic signal).
     * @param handle The coroutine handle to resume
     * @param task_id Tracked-task id for registry progress, or -1 if untracked.
     */
    void enqueue(std::coroutine_handle<> handle, TaskIndex task_id = -1);

    /**
     * Enqueue a Coro with progress tracking in task_registry_.
     */
    TaskIndex enqueue_tracked(
        coro::Coro coro, std::string name,
        std::shared_ptr<std::atomic<TaskIndex>> tid_out = nullptr);

    void mark_coro_completed(TaskIndex id);

    /**
     * Submit a Task for execution via a Coro (Phase 3 path).
     * Creates a run_task() Coro, registers in task_registry_,
     * and enqueues the released handle to run_queue_.
     * @param task The DAG task to execute
     * @param input Task input
     * @param parent_task_id Parent task ID for tracking (-1 for root)
     */
    void submit_task(std::shared_ptr<Task> task,
                     std::shared_ptr<std::any> input,
                     TaskIndex parent_task_id = -1);

    /**
     * Schedule a completed Coro handle for deferred destruction.
     * Called from CoroPromise::FinalAwaiter for released handles.
     * @param handle The coroutine handle at final_suspend
     */
    void schedule_destroy(std::coroutine_handle<> handle);

   private:
    /**
     * Worker thread main loop
     */
    void worker_thread(WorkerContext* context);

    /**
     * Update task location in registry
     */
    void update_task_location(TaskIndex task_id, TaskInfo::Location location,
                              std::size_t worker_id);

    /**
     * Build task progress tree recursively
     */
    TaskProgress build_task_progress_tree(
        TaskIndex task_id, std::unordered_set<TaskIndex>& processed) const;

    /**
     * Notify completion callback
     */
    void notify_completion(std::shared_ptr<Task> task);

    /**
     * Mark activity (task start or completion) for responsiveness tracking
     */
    void mark_activity();

    /**
     * Signal workers that new global work is available.
     */
    void signal_global_work();

    /**
     * Wake one worker thread.
     */
    void wake_one_worker();

    /**
     * Wake all worker threads.
     */
    void wake_all_workers();

    /**
     * Create a Coro that executes a Task with full bookkeeping.
     * The returned Coro is at initial_suspend -- caller must
     * set executor on promise and enqueue via release().
     */
    coro::Coro run_task(std::shared_ptr<Task> task,
                        std::shared_ptr<std::any> input);

    /**
     * Drain the destroy queue (called from worker loop).
     */
    void drain_destroy_queue();

    friend class Scheduler;
    friend struct coro::CoroPromise;
};

void* get_current_worker_context();
void set_current_worker_context(void* context);

/// Push a coroutine handle onto the current worker's thread-local
/// destroy list.  The worker drains this list after each resume(),
/// guaranteeing the frame is fully suspended before destruction.
void schedule_thread_local_destroy(std::coroutine_handle<> h);

/// Drain the current thread's pending-destroy list.
void drain_thread_local_destroys();

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTOR_H
