#ifndef DFTRACER_UTILS_CORE_PIPELINE_WATCHDOG_H
#define DFTRACER_UTILS_CORE_PIPELINE_WATCHDOG_H

#include <dftracer/utils/core/common/typedefs.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace dftracer::utils {

class Task;
class Executor;

/**
 * Watchdog - Monitors pipeline execution and enforces timeouts
 *
 * Responsibilities:
 * - Monitor global pipeline timeout
 * - Monitor per-task timeouts
 * - Detect when executor becomes unresponsive
 * - Log warnings for long-running tasks
 * - Trigger graceful shutdown on timeout
 *
 * The watchdog runs in its own thread, independent of the executor
 * and scheduler, so it can detect hangs even if they are stuck.
 */
class Watchdog {
   public:
    /**
     * Callback when timeout is detected
     * Parameters: reason (string describing what timed out)
     */
    using TimeoutCallback = std::function<void(const std::string& reason)>;

    /**
     * Callback for warnings about long-running tasks
     * Parameters: task name, elapsed time in milliseconds
     */
    using WarningCallback =
        std::function<void(const std::string& task_name, int64_t elapsed_ms)>;

    /**
     * Information about an active task execution
     */
    struct TaskExecution {
        std::shared_ptr<Task> task;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::milliseconds timeout;  // 0 = no timeout
        bool warning_logged{false};         // Track if warning already logged
    };

   private:
    // Configuration
    std::chrono::milliseconds check_interval_;
    std::chrono::milliseconds global_timeout_;
    std::chrono::milliseconds default_task_timeout_;
    std::chrono::milliseconds warning_threshold_;

    // Execution tracking
    std::chrono::steady_clock::time_point execution_start_time_;
    std::unordered_map<TaskIndex, TaskExecution> active_tasks_;
    mutable std::mutex active_tasks_mutex_;

    // Thread management
    std::thread watchdog_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};

    // Callbacks
    TimeoutCallback timeout_callback_;
    WarningCallback warning_callback_;

    // Reference to executor (for responsiveness check)
    Executor* executor_{nullptr};

   public:
    /**
     * Constructor
     * @param check_interval How often to check for timeouts
     * @param global_timeout Global pipeline timeout (0 = no timeout)
     * @param default_task_timeout Default per-task timeout (0 = no timeout)
     * @param warning_threshold Warn about tasks running longer than this
     */
    explicit Watchdog(
        std::chrono::milliseconds check_interval =
            std::chrono::milliseconds(100),
        std::chrono::milliseconds global_timeout = std::chrono::milliseconds(0),
        std::chrono::milliseconds default_task_timeout =
            std::chrono::milliseconds(0),
        std::chrono::milliseconds warning_threshold =
            std::chrono::milliseconds(10000));

    ~Watchdog();

    // Prevent copying and moving
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    /**
     * Start the watchdog thread
     */
    void start();

    /**
     * Stop the watchdog thread
     */
    void stop();

    /**
     * Mark the start of pipeline execution
     */
    void mark_execution_start();

    /**
     * Register a task as actively executing
     * @param task_id Task identifier
     * @param task Shared pointer to the task
     * @param timeout Task-specific timeout (0 = use default or no timeout)
     */
    void register_task_start(
        TaskIndex task_id, std::shared_ptr<Task> task,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    /**
     * Unregister a task (called when it completes)
     * @param task_id Task identifier
     */
    void unregister_task(TaskIndex task_id);

    /**
     * Set callback for timeout events
     */
    void set_timeout_callback(TimeoutCallback callback);

    /**
     * Set callback for warning events
     */
    void set_warning_callback(WarningCallback callback);

    /**
     * Set executor reference for responsiveness checks
     */
    void set_executor(Executor* executor);

    /**
     * Set global timeout
     */
    void set_global_timeout(std::chrono::milliseconds timeout);

    /**
     * Set default task timeout
     */
    void set_default_task_timeout(std::chrono::milliseconds timeout);

    /**
     * Check if watchdog is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * Check if shutdown was requested
     */
    bool is_shutdown_requested() const { return shutdown_requested_.load(); }

    /**
     * Get number of active tasks being monitored
     */
    size_t get_active_task_count() const;

   private:
    /**
     * Main watchdog loop - runs in separate thread
     */
    void watchdog_loop();

    /**
     * Check if global timeout has been exceeded
     * @return true if timed out
     */
    bool check_global_timeout();

    /**
     * Check all active tasks for timeouts and warnings
     * @return true if any task timed out
     */
    bool check_task_timeouts();

    /**
     * Check if executor is responsive
     * @return true if unresponsive
     */
    bool check_executor_responsiveness();

    /**
     * Trigger timeout callback
     */
    void trigger_timeout(const std::string& reason);

    /**
     * Trigger warning callback
     */
    void trigger_warning(const std::string& task_name, int64_t elapsed_ms);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_WATCHDOG_H
