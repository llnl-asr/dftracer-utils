#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTOR_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/task_queue.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
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
class TaskContext;
class Scheduler;

/**
 * Task submission hint for queue selection
 */
enum class SubmissionHint {
    AUTO,          // Let executor decide (default)
    FORCE_SHARED,  // Force submission to shared queue
    FORCE_LOCAL    // Force submission to local queue (if possible)
};

/**
 * Task information for progress tracking
 */
struct TaskInfo {
    TaskIndex task_id;
    TaskIndex parent_task_id;  // -1 for root tasks
    std::string name;
    size_t worker_id;          // Which worker is executing

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
    std::atomic<size_t> completed_children{0};

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
    size_t total_subtasks;
    size_t completed_subtasks;
    double progress_percentage;  // 0-100

    // Location
    std::string
        location;  // "shared_queue", "worker_2_local", "executing_on_worker_3"

    // Children
    std::vector<TaskProgress> children;  // Recursive structure!
};

/**
 * Executor progress report
 */
struct ExecutorProgress {
    // Overall stats
    size_t total_tasks_submitted;
    size_t tasks_queued;
    size_t tasks_running;
    size_t tasks_completed;
    size_t tasks_failed;

    // Queue depths
    size_t shared_queue_depth;
    std::vector<size_t> worker_queue_depths;

    // Task tree (root tasks with their children)
    std::vector<TaskProgress> root_tasks;

    // Worker states
    struct WorkerStatus {
        size_t worker_id;
        bool is_idle;
        std::optional<TaskIndex> current_task_id;
        std::string current_task_name;
        size_t local_queue_depth;
    };
    std::vector<WorkerStatus> workers;

    // Performance metrics
    double avg_queue_wait_ms;
    double avg_execution_time_ms;
    size_t total_tasks_stolen;

    // Errors
    std::vector<std::pair<TaskIndex, std::string>>
        recent_errors;  // task_id, error_msg
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
    // Worker context for per-thread state
    struct WorkerContext {
        size_t worker_id;
        std::deque<TaskItem> local_queue;  // Worker's private queue
        mutable std::mutex queue_mutex;    // Protects local queue
        std::condition_variable cv;        // For waking up worker

        // Health monitoring for watchdog
        std::atomic<bool> is_alive{true};
        std::atomic<bool> is_idle{false};
        std::atomic<uint64_t> tasks_executed{0};
        std::atomic<uint64_t> tasks_stolen{0};
        std::chrono::steady_clock::time_point last_activity;

        // Current task info (for debugging/watchdog)
        std::atomic<TaskIndex> current_task_id{-1};
        std::string current_task_name;
        std::mutex task_name_mutex;  // Protects current_task_name

        // Worker thread
        std::thread thread;

        explicit WorkerContext(size_t id) : worker_id(id) {}
    };

    // Shared queue for external submissions
    TaskQueue shared_queue_;  // Renamed from queue_

    // Per-worker contexts
    std::vector<std::unique_ptr<WorkerContext>> workers_;
    std::atomic<size_t> next_worker_{0};  // For round-robin submission

    std::atomic<bool> running_{false};
    size_t num_threads_;

    CompletionCallback completion_callback_;
    std::mutex callback_mutex_;

    // Reference to scheduler for dynamic task context
    Scheduler* scheduler_{nullptr};

    // Global tracking
    std::atomic<size_t> tasks_completed_{0};
    std::atomic<size_t> tasks_started_{0};
    std::atomic<size_t> total_tasks_submitted_{0};
    std::atomic<size_t> total_tasks_stolen_{0};

    std::chrono::steady_clock::time_point last_activity_time_;
    mutable std::mutex activity_mutex_;

    // Shutdown coordination
    std::atomic<bool> shutdown_requested_{false};

    // Task registry for progress tracking
    std::unordered_map<TaskIndex, TaskInfo> task_registry_;
    mutable std::shared_mutex registry_mutex_;  // Allow concurrent reads

   public:
    /**
     * Constructor
     * @param num_threads Number of worker threads (0 = hardware_concurrency)
     */
    explicit Executor(size_t num_threads = 0);

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
     * Get reference to the shared task queue
     */
    TaskQueue& get_queue() { return shared_queue_; }

    /**
     * Set completion callback (called when task finishes)
     */
    void set_completion_callback(CompletionCallback callback);

    /**
     * Set scheduler reference (for TaskContext)
     */
    void set_scheduler(Scheduler* scheduler) { scheduler_ = scheduler; }

    /**
     * Check if executor is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * Get number of worker threads
     */
    size_t get_num_threads() const { return num_threads_; }

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
     * Submit a task with tracking context
     * @param item Task to submit
     * @param parent_task_id Parent task ID (-1 for root tasks)
     * @param hint Submission hint for queue selection
     */
    void submit_with_context(const TaskItem& item,
                             TaskIndex parent_task_id = -1,
                             SubmissionHint hint = SubmissionHint::AUTO);

    /**
     * Get full progress report
     */
    ExecutorProgress get_progress() const;

    /**
     * Get progress for a specific task and its subtree
     */
    std::optional<TaskProgress> get_task_progress(TaskIndex task_id) const;

   private:
    /**
     * Try to steal and execute one task from the queue (internal use)
     * Returns true if a task was executed
     */
    bool try_steal_one_task();

    /**
     * Worker thread main loop (new version with per-worker context)
     */
    void worker_thread(WorkerContext* context);

    /**
     * Execute a single task
     */
    void execute_task(WorkerContext* context, TaskItem& item);

    /**
     * Try to pop from worker's local queue
     */
    bool try_pop_local(WorkerContext* context, TaskItem& item);

    /**
     * Try to steal from other workers
     */
    bool try_steal_from_others(WorkerContext* thief, TaskItem& item);

    /**
     * Update task location in registry
     */
    void update_task_location(TaskIndex task_id, TaskInfo::Location location,
                              size_t worker_id);

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

    friend class TaskFuture;
    friend class Scheduler;
};

// Helper functions for work-stealing support
Executor* get_current_executor();
void set_current_executor(Executor* exec);

// Helper functions for worker context (thread-local) - internal use
void* get_current_worker_context();
void set_current_worker_context(void* context);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTOR_H
