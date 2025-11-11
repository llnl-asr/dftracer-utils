#ifndef DFTRACER_UTILS_CORE_PIPELINE_SCHEDULER_H
#define DFTRACER_UTILS_CORE_PIPELINE_SCHEDULER_H

#include <concurrentqueue.h>
#include <dftracer/utils/core/common/sharded_mutex.h>
#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils {

class Task;
class Executor;
struct ExecutorProgress;
class Watchdog;

/**
 * Task priority levels for scheduling
 * Lower values = higher priority
 */
enum class TaskPriority {
    CRITICAL = 0,   // Highest priority - critical path tasks
    HIGH = 1,       // High priority - important tasks
    NORMAL = 2,     // Default priority
    LOW = 3,        // Low priority
    BACKGROUND = 4  // Lowest priority - background tasks
};

/**
 * Comprehensive scheduler metrics including executor progress
 */
struct SchedulerMetrics {
    // Scheduling performance
    std::size_t ready_queue_depth;
    std::size_t total_scheduled;
    std::size_t scheduling_threads_active;
    double avg_scheduling_latency_ms;

    // Pipeline state
    std::size_t total_pipeline_tasks;
    std::size_t pending_tasks;
    double pipeline_elapsed_time_ms;

    // Error tracking
    std::vector<std::pair<TaskIndex, std::string>> recent_failures;

    // Priority distribution
    std::map<TaskPriority, std::size_t> tasks_by_priority;

    // Timing
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update;
};

/**
 * Scheduler - Manages task scheduling and dependency tracking
 *
 * Features:
 * - Small thread pool for lightweight scheduling (1-2 threads)
 * - Traverses DAG and checks dependencies
 * - Submits ready tasks to executor queue
 * - Tracks completed tasks
 * - Handles error policies
 * - Supports dynamic task submission
 */
class Scheduler {
   private:
    Executor* executor_;  // Reference to executor
    std::atomic<bool> running_{false};

    std::vector<std::thread> scheduling_threads_;
    std::atomic<bool> scheduling_running_{false};
    std::size_t num_scheduling_threads_{1};

    // Task queue for scheduling (tasks that became ready) - lock-free MPMC
    moodycamel::ConcurrentQueue<std::shared_ptr<Task>> ready_queue_;
    std::atomic<bool> has_ready_tasks_{false};

    // Watchdog integration
    std::unique_ptr<Watchdog> watchdog_;

    // Timeout configuration
    std::chrono::milliseconds global_timeout_{0};
    std::chrono::milliseconds default_task_timeout_{0};

    // Shutdown coordination
    std::atomic<bool> shutdown_requested_{false};

    // Execution start time (for global timeout)
    std::chrono::steady_clock::time_point execution_start_time_;

    // Mutex for coordinating task tracking operations
    mutable std::mutex tracking_mutex_;

    // Pending count for execution coordination
    std::atomic<std::size_t> pending_count_{0};
    std::condition_variable done_cv_;
    std::mutex done_mutex_;

    // Error policy
    ErrorPolicy error_policy_{ErrorPolicy::FAIL_FAST};
    ErrorHandler error_handler_;
    std::atomic<bool> has_error_{false};
    // Reason for timeout (if timeout occurred)
    std::string timeout_reason_;

    // Progress callback
    std::function<void(std::size_t completed, std::size_t total)>
        progress_callback_;
    std::atomic<std::size_t> total_tasks_{0};

    // Metrics tracking
    std::atomic<std::size_t> total_scheduled_{
        0};  // Total tasks scheduled to executor
    std::chrono::steady_clock::time_point metrics_start_time_;
    mutable std::mutex metrics_mutex_;

    // Coroutine support - completion callbacks
    using CallbackMap =
        std::unordered_map<TaskIndex, std::vector<std::function<void()>>>;
    ShardedMutex<CallbackMap, 64> completion_callbacks_;

   public:
    /**
     * Constructor
     * @param executor Reference to executor
     */
    explicit Scheduler(Executor* executor);

    /**
     * Constructor with configuration
     * @param executor Reference to executor
     * @param config Pipeline configuration
     */
    explicit Scheduler(Executor* executor, const PipelineConfig& config);

    ~Scheduler();

    // Prevent copying
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Prevent moving
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    /**
     * Schedule execution starting from source task
     * @param source Starting task
     * @param input Initial input
     */
    void schedule(std::shared_ptr<Task> source, const std::any& input = {});

    /**
     * Schedule execution starting from source task
     * @param source Starting task
     * @param input Initial input
     */
    template <typename T, typename = std::enable_if_t<
                              !std::is_same_v<std::decay_t<T>, std::any>>>
    void schedule(std::shared_ptr<Task> source, T&& input) {
        schedule(source, std::any(std::forward<T>(input)));
    }

    /**
     * Called by executor when a task completes
     * @param task Completed task
     */
    void on_task_completed(std::shared_ptr<Task> task);

    /**
     * Submit a dynamic task (for intra-task parallelism)
     * @param task Task to submit
     * @param input Task input
     */
    void submit_dynamic_task(std::shared_ptr<Task> task, const std::any& input);

    /**
     * Set error handling policy
     */
    void set_error_policy(ErrorPolicy policy);

    /**
     * Set custom error handler
     */
    void set_error_handler(ErrorHandler handler);

    /**
     * Set progress callback
     */
    void set_progress_callback(
        std::function<void(std::size_t completed, std::size_t total)> callback);

    /**
     * Set global timeout (0 = wait forever)
     */
    void set_global_timeout(std::chrono::milliseconds timeout);

    /**
     * Set default task timeout (0 = wait forever)
     */
    void set_default_task_timeout(std::chrono::milliseconds timeout);

    /**
     * Request graceful shutdown
     */
    void request_shutdown();

    /**
     * Check if shutdown was requested
     */
    bool is_shutdown_requested() const { return shutdown_requested_.load(); }

    /**
     * Get watchdog (for configuration)
     */
    Watchdog* get_watchdog() { return watchdog_.get(); }

    /**
     * Reset scheduler state
     */
    void reset();

    /**
     * Check if task has completed
     */
    bool is_task_completed(TaskIndex task_id) const;

    /**
     * Check if scheduler is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * Get comprehensive metrics including executor progress
     */
    SchedulerMetrics get_metrics() const;

    /**
     * Get executor progress (convenience method)
     */
    ExecutorProgress get_executor_progress() const;

    /**
     * Get executor reference (for TaskFuture async suspension)
     */
    Executor* get_executor() const { return executor_; }

    // ========================================================================
    // Coroutine Support - Completion Callbacks
    // ========================================================================

    /**
     * Register callback to be invoked when task completes
     *
     * OPTIMIZED: Uses ShardedMutex for minimal contention.
     * Multiple tasks can register callbacks concurrently without blocking.
     *
     * @param task_id Task identifier
     * @param callback Function to invoke on completion: []() { ... }
     *
     * Thread-safe: Only locks the specific shard for this task_id.
     * Other tasks registering callbacks concurrently don't block.
     *
     * Usage:
     * @code
     * // Called from TaskFuture::await_suspend()
     * scheduler->register_task_completion_callback(task_id, [awaiting]() {
     *     awaiting.resume();  // Resume awaiting coroutine
     * });
     * @endcode
     */
    void register_task_completion_callback(TaskIndex task_id,
                                           std::function<void()> callback);

    /**
     * Invoke completion callbacks for a task
     *
     * OPTIMIZED: Only locks the specific shard for this task_id.
     * Other tasks completing concurrently don't block.
     *
     * @param task_id Task identifier
     *
     * Called by on_task_completed() when a task finishes.
     * Invokes all registered callbacks (e.g., resuming awaiting coroutines).
     */
    void invoke_completion_callbacks(TaskIndex task_id);

   private:
    /**
     * Validate task graph types (called before scheduling)
     */
    void validate_task_types(std::shared_ptr<Task> task);

    /**
     * Schedule children of a completed task
     */
    void schedule_ready_children(std::shared_ptr<Task> completed_task);

    /**
     * Check if all parents of a task have completed
     */
    bool all_parents_completed(std::shared_ptr<Task> task) const;

    /**
     * Prepare input for a task from its parents' outputs
     */
    std::any prepare_input_for_task(std::shared_ptr<Task> task);

    /**
     * Submit a task to the executor queue
     */
    void submit_task_to_executor(std::shared_ptr<Task> task,
                                 const std::any& input);

    /**
     * Count total tasks reachable from source
     */
    std::size_t count_reachable_tasks(std::shared_ptr<Task> source);

    /**
     * DFS helper for counting tasks
     */
    void count_tasks_dfs(std::shared_ptr<Task> task,
                         std::unordered_set<TaskIndex>& visited,
                         std::size_t& count);

    /**
     * Initialize pending counts for all tasks
     */
    void initialize_pending_counts(std::shared_ptr<Task> source);

    /**
     * DFS helper for initializing pending counts
     */
    void initialize_pending_counts_dfs(std::shared_ptr<Task> task,
                                       std::unordered_set<TaskIndex>& visited);

    /**
     * Handle task error
     */
    void handle_task_error(std::shared_ptr<Task> task);

    /**
     * Skip a task and all its descendants (recursive)
     * Used in CONTINUE/CUSTOM error policy
     */
    void skip_task_and_descendants(std::shared_ptr<Task> task);

    /**
     * Scheduling loop - runs in separate thread
     */
    void scheduling_loop();

    /**
     * Process a ready task
     */
    void process_ready_task(std::shared_ptr<Task> task);

    /**
     * Start scheduling thread
     */
    void start_scheduling_thread();

    /**
     * Stop scheduling thread
     */
    void stop_scheduling_thread();
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_SCHEDULER_H
