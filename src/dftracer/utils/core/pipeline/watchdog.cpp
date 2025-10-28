#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/tasks/task.h>

namespace dftracer::utils {

Watchdog::Watchdog(std::chrono::milliseconds check_interval,
                   std::chrono::milliseconds global_timeout,
                   std::chrono::milliseconds default_task_timeout,
                   std::chrono::milliseconds warning_threshold)
    : check_interval_(check_interval),
      global_timeout_(global_timeout),
      default_task_timeout_(default_task_timeout),
      warning_threshold_(warning_threshold) {
    DFTRACER_UTILS_LOG_DEBUG(
        "Watchdog created: check_interval=%lld ms, global_timeout=%lld ms, "
        "task_timeout=%lld ms, warning_threshold=%lld ms",
        check_interval_.count(), global_timeout_.count(),
        default_task_timeout_.count(), warning_threshold_.count());
}

Watchdog::~Watchdog() { stop(); }

void Watchdog::start() {
    if (running_.load()) {
        DFTRACER_UTILS_LOG_WARN("%s", "Watchdog already running");
        return;
    }

    running_ = true;
    shutdown_requested_ = false;
    watchdog_thread_ = std::thread(&Watchdog::watchdog_loop, this);

    DFTRACER_UTILS_LOG_DEBUG("%s", "Watchdog started");
}

void Watchdog::stop() {
    if (!running_.load()) {
        return;
    }

    running_ = false;

    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Watchdog stopped");
}

void Watchdog::mark_execution_start() {
    execution_start_time_ = std::chrono::steady_clock::now();

    DFTRACER_UTILS_LOG_DEBUG("%s", "Watchdog: execution started");
}

void Watchdog::register_task_start(TaskIndex task_id,
                                   std::shared_ptr<Task> task,
                                   std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);

    // Use task-specific timeout, or default, or no timeout
    std::chrono::milliseconds effective_timeout = timeout;
    if (effective_timeout.count() == 0) {
        effective_timeout = default_task_timeout_;
    }

    TaskExecution exec;
    exec.task = task;
    exec.start_time = std::chrono::steady_clock::now();
    exec.timeout = effective_timeout;
    exec.warning_logged = false;
    active_tasks_[task_id] = exec;

    DFTRACER_UTILS_LOG_DEBUG(
        "Watchdog: registered task '%s' (ID: %llu) with timeout: %lld ms",
        task->get_name().c_str(), task_id, effective_timeout.count());
}

void Watchdog::unregister_task(TaskIndex task_id) {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);

    auto it = active_tasks_.find(task_id);
    if (it != active_tasks_.end()) {
        DFTRACER_UTILS_LOG_DEBUG("Watchdog: unregistered task '%s' (ID: %llu)",
                                 it->second.task->get_name().c_str(), task_id);

        active_tasks_.erase(it);
    }
}

void Watchdog::set_timeout_callback(TimeoutCallback callback) {
    timeout_callback_ = std::move(callback);
}

void Watchdog::set_warning_callback(WarningCallback callback) {
    warning_callback_ = std::move(callback);
}

void Watchdog::set_executor(Executor* executor) { executor_ = executor; }

void Watchdog::set_global_timeout(std::chrono::milliseconds timeout) {
    global_timeout_ = timeout;
}

void Watchdog::set_default_task_timeout(std::chrono::milliseconds timeout) {
    default_task_timeout_ = timeout;
}

size_t Watchdog::get_active_task_count() const {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);
    return active_tasks_.size();
}

void Watchdog::watchdog_loop() {
    DFTRACER_UTILS_LOG_DEBUG("%s", "Watchdog loop started");

    while (running_.load()) {
        // Sleep for check interval
        std::this_thread::sleep_for(check_interval_);

        if (!running_.load() || shutdown_requested_.load()) {
            break;
        }

        // Check global timeout
        if (check_global_timeout()) {
            DFTRACER_UTILS_LOG_ERROR("%s", "Watchdog detected global timeout");
            break;
        }

        // Check task timeouts
        if (check_task_timeouts()) {
            DFTRACER_UTILS_LOG_ERROR("%s", "Watchdog detected task timeout");
            break;
        }

        // Check executor responsiveness
        if (check_executor_responsiveness()) {
            DFTRACER_UTILS_LOG_ERROR("%s",
                                     "Watchdog detected unresponsive executor");
            trigger_timeout("Executor became unresponsive");
            break;
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Watchdog loop ended", "");
}

bool Watchdog::check_global_timeout() {
    // Skip if no global timeout set
    if (global_timeout_.count() == 0) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - execution_start_time_;

    if (elapsed > global_timeout_) {
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

        DFTRACER_UTILS_LOG_ERROR(
            "Global timeout exceeded: %lld ms elapsed (limit: %lld ms)",
            elapsed_ms.count(), global_timeout_.count());

        trigger_timeout("Global pipeline timeout after " +
                        std::to_string(elapsed_ms.count()) + " ms (limit: " +
                        std::to_string(global_timeout_.count()) + " ms)");

        return true;
    }

    return false;
}

bool Watchdog::check_task_timeouts() {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);

    auto now = std::chrono::steady_clock::now();

    for (auto& [task_id, execution] : active_tasks_) {
        auto elapsed = now - execution.start_time;
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

        // Check timeout
        if (execution.timeout.count() > 0 && elapsed > execution.timeout) {
            DFTRACER_UTILS_LOG_ERROR(
                "Task timeout: '%s' (ID: %llu) ran for %lld ms (limit: %lld "
                "ms)",
                execution.task->get_name().c_str(), task_id, elapsed_ms.count(),
                execution.timeout.count());

            trigger_timeout(
                "Task '" + execution.task->get_name() + "' timed out after " +
                std::to_string(elapsed_ms.count()) + " ms (limit: " +
                std::to_string(execution.timeout.count()) + " ms)");

            return true;
        }

        // Check warning threshold
        if (warning_threshold_.count() > 0 && !execution.warning_logged &&
            elapsed > warning_threshold_) {
            execution.warning_logged = true;

            DFTRACER_UTILS_LOG_WARN(
                "Long-running task: '%s' (ID: %llu) has been running for %lld "
                "ms",
                execution.task->get_name().c_str(), task_id,
                elapsed_ms.count());

            trigger_warning(execution.task->get_name(), elapsed_ms.count());
        }
    }

    return false;
}

bool Watchdog::check_executor_responsiveness() {
    // Skip if no executor reference
    if (!executor_) {
        return false;
    }

    // Use executor's built-in responsiveness check
    // This checks for:
    // - Executor is running
    // - Queue is being processed (tasks completing)
    // - No apparent deadlock (all threads stuck)
    // - Making progress on active tasks

    if (!executor_->is_responsive()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Executor is unresponsive (hung, deadlocked, or crashed)");
        return true;
    }

    return false;
}

void Watchdog::trigger_timeout(const std::string& reason) {
    shutdown_requested_ = true;

    if (timeout_callback_) {
        timeout_callback_(reason);
    }
}

void Watchdog::trigger_warning(const std::string& task_name,
                               int64_t elapsed_ms) {
    if (warning_callback_) {
        warning_callback_(task_name, elapsed_ms);
    }
}

}  // namespace dftracer::utils
