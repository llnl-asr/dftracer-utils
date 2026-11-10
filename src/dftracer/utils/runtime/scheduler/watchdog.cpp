// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/watchdog.h>

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
        "Watchdog created: check_interval={} ms, global_timeout={} ms, "
        "task_timeout={} ms, warning_threshold={} ms",
        static_cast<long long>(check_interval_.count()),
        static_cast<long long>(global_timeout_.count()),
        static_cast<long long>(default_task_timeout_.count()),
        static_cast<long long>(warning_threshold_.count()));
}

Watchdog::~Watchdog() { stop(); }

void Watchdog::start() {
    if (running_.load()) {
        DFTRACER_UTILS_LOG_WARN("{}", "Watchdog already running");
        return;
    }

    running_ = true;
    shutdown_requested_ = false;
    watchdog_thread_ = dstd::Thread([this] { watchdog_loop(); });

    DFTRACER_UTILS_LOG_DEBUG("{}", "Watchdog started");
}

void Watchdog::stop() {
    if (!running_.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sleep_mutex_);
        running_ = false;
        sleep_cv_.notify_one();
    }

    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }

    DFTRACER_UTILS_LOG_DEBUG("{}", "Watchdog stopped");
}

void Watchdog::mark_execution_start() {
    execution_start_time_ = std::chrono::steady_clock::now();

    DFTRACER_UTILS_LOG_DEBUG("{}", "Watchdog: execution started");
}

void Watchdog::register_task_start(common::TaskIndex task_id,
                                   dstd::StringView name,
                                   std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);

    // Use task-specific timeout, or default, or no timeout
    std::chrono::milliseconds effective_timeout = timeout;
    if (effective_timeout.count() == 0) {
        effective_timeout = default_task_timeout_;
    }

    TaskExecution exec;
    exec.name = dstd::String(name);
    exec.start_time = std::chrono::steady_clock::now();
    exec.timeout = effective_timeout;
    exec.warning_logged = false;
    active_tasks_[task_id] = exec;

    DFTRACER_UTILS_LOG_DEBUG(
        "Watchdog: registered task '{}' (ID: {}) with timeout: {} ms",
        exec.name, task_id, static_cast<long long>(effective_timeout.count()));
}

void Watchdog::unregister_task(common::TaskIndex task_id) {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);

    auto it = active_tasks_.find(task_id);
    if (it != active_tasks_.end()) {
        DFTRACER_UTILS_LOG_DEBUG("Watchdog: unregistered task '{}' (ID: {})",
                                 it->second.name, task_id);

        active_tasks_.erase(it);
    }
}

void Watchdog::set_timeout_callback(TimeoutCallback callback) {
    timeout_callback_ = std::move(callback);
}

void Watchdog::set_warning_callback(WarningCallback callback) {
    warning_callback_ = std::move(callback);
}

void Watchdog::set_executor(TaskExecutor* executor) { executor_ = executor; }

void Watchdog::set_global_timeout(std::chrono::milliseconds timeout) {
    global_timeout_ = timeout;
}

void Watchdog::set_default_task_timeout(std::chrono::milliseconds timeout) {
    default_task_timeout_ = timeout;
}

size_t Watchdog::active_task_count() const {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);
    return active_tasks_.size();
}

void Watchdog::watchdog_loop() {
    DFTRACER_UTILS_LOG_DEBUG("{}", "Watchdog loop started");

    while (running_.load()) {
        // Interruptible sleep using condition variable
        {
            std::unique_lock<std::mutex> lock(sleep_mutex_);
            sleep_cv_.wait_for(lock, check_interval_,
                               [this] { return !running_.load(); });
        }

        if (!running_.load() || shutdown_requested_.load()) {
            break;
        }

        // Check global timeout
        if (check_global_timeout()) {
            DFTRACER_UTILS_LOG_ERROR("{}", "Watchdog detected global timeout");
            break;
        }

        // Check task timeouts
        if (check_task_timeouts()) {
            DFTRACER_UTILS_LOG_ERROR("{}", "Watchdog detected task timeout");
            break;
        }

        // Check executor responsiveness
        if (check_executor_responsiveness()) {
            DFTRACER_UTILS_LOG_ERROR("{}",
                                     "Watchdog detected unresponsive executor");
            trigger_timeout("Executor became unresponsive");
            break;
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Watchdog loop ended");
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
            "Global timeout exceeded: {} ms elapsed (limit: {} ms)",
            static_cast<long long>(elapsed_ms.count()),
            static_cast<long long>(global_timeout_.count()));

        trigger_timeout(dstd::fmt<"Global timeout after {} ms (limit: {} ms)">(
            elapsed_ms.count(), global_timeout_.count()));

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
                "Task timeout: '{}' (ID: {}) ran for {} ms (limit: {} ms)",
                execution.name, task_id,
                static_cast<long long>(elapsed_ms.count()),
                static_cast<long long>(execution.timeout.count()));

            {
                char buf[1024];
                std::snprintf(
                    buf, sizeof(buf),
                    "Task '%s' timed out after %lld ms (limit: %lld ms)",
                    execution.name.c_str(),
                    static_cast<long long>(elapsed_ms.count()),
                    static_cast<long long>(execution.timeout.count()));
                trigger_timeout(buf);
            }

            return true;
        }

        // Check warning threshold
        if (warning_threshold_.count() > 0 && !execution.warning_logged &&
            elapsed > warning_threshold_) {
            execution.warning_logged = true;

            DFTRACER_UTILS_LOG_WARN(
                "Long-running task: '{}' (ID: {}) has been running for {} ms",
                execution.name, task_id,
                static_cast<long long>(elapsed_ms.count()));

            trigger_warning(execution.name.c_str(), elapsed_ms.count());
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
            "{}", "Executor is unresponsive (hung, deadlocked, or crashed)");
        return true;
    }

    return false;
}

void Watchdog::trigger_timeout(dstd::StringView reason) {
    shutdown_requested_ = true;

    if (timeout_callback_) {
        timeout_callback_(reason);
    }
}

void Watchdog::trigger_warning(dstd::StringView task_name, int64_t elapsed_ms) {
    if (warning_callback_) {
        warning_callback_(task_name, elapsed_ms);
    }
}

}  // namespace dftracer::utils
