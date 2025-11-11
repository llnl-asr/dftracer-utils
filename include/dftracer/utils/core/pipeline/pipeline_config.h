#ifndef DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_CONFIG_H
#define DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_CONFIG_H

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace dftracer::utils {

// Forward declarations
class Task;

/**
 * Error handling policy for pipeline execution
 */
enum class ErrorPolicy {
    FAIL_FAST,  // Stop immediately on first error (default)
    CONTINUE,   // Continue other branches, skip children of failed tasks
    CUSTOM      // User-provided handler
};

/**
 * Error handler function type
 * Parameters: (failed_task, exception_ptr)
 */
using ErrorHandler =
    std::function<void(std::shared_ptr<Task>, std::exception_ptr)>;

/**
 * Configuration  for Pipeline execution
 *
 * Thread Architecture:
 * - Executor threads: Worker pool that executes task functions
 * - Scheduler threads: Coordination and monitoring (usually 1)
 * - Watchdog: Optional monitoring thread for hang detection
 *
 * Usage (Fluent API):
 *   auto config = PipelineConfig()
 *       .with_name("MyPipeline")
 *       .with_executor_threads(4)
 *       .with_scheduler_threads(2)
 *       .with_error_policy(ErrorPolicy::FAIL_FAST)
 *       .with_watchdog(true)
 *       .with_global_timeout(std::chrono::seconds(30))
 *       .with_task_timeout(std::chrono::seconds(10));
 */
struct PipelineConfig {
    std::string name = "";              // Pipeline name
    std::size_t executor_threads = 0;   // 0 = hardware_concurrency
    std::size_t scheduler_threads = 1;  // Usually 1
    ErrorPolicy error_policy = ErrorPolicy::FAIL_FAST;  // Error handling policy
    ErrorHandler error_handler =
        nullptr;                  // Custom error handler (for CUSTOM policy)
    bool enable_watchdog = true;  // Hang detection
    std::chrono::seconds global_timeout{0};        // 0 = wait forever
    std::chrono::seconds default_task_timeout{0};  // 0 = wait forever
    std::chrono::seconds watchdog_interval{1};     // Check frequency
    std::chrono::seconds long_task_warning_threshold{
        300};  // Warning threshold (5 minutes)
    std::chrono::seconds executor_idle_timeout{
        300};  // Executor idle timeout (5 minutes)
    std::chrono::seconds executor_deadlock_timeout{
        600};  // Executor deadlock timeout (10 minutes)

    /**
     * Set pipeline name
     */
    PipelineConfig& with_name(std::string pipeline_name) {
        name = std::move(pipeline_name);
        return *this;
    }

    /**
     * Set number of executor threads
     */
    PipelineConfig& with_executor_threads(std::size_t threads) {
        executor_threads = threads;
        return *this;
    }

    /**
     * Set number of scheduler threads
     */
    PipelineConfig& with_scheduler_threads(std::size_t threads) {
        scheduler_threads = threads;
        return *this;
    }

    /**
     * Set error handling policy
     */
    PipelineConfig& with_error_policy(ErrorPolicy policy) {
        error_policy = policy;
        return *this;
    }

    /**
     * Set custom error handler (automatically sets policy to CUSTOM)
     */
    PipelineConfig& with_error_handler(ErrorHandler handler) {
        error_handler = std::move(handler);
        error_policy = ErrorPolicy::CUSTOM;
        return *this;
    }

    /**
     * Enable/disable watchdog
     */
    PipelineConfig& with_watchdog(bool enabled) {
        enable_watchdog = enabled;
        return *this;
    }

    /**
     * Set global timeout (0 = wait forever)
     */
    PipelineConfig& with_global_timeout(std::chrono::seconds timeout) {
        global_timeout = timeout;
        return *this;
    }

    /**
     * Set default task timeout (0 = wait forever)
     */
    PipelineConfig& with_task_timeout(std::chrono::seconds timeout) {
        default_task_timeout = timeout;
        return *this;
    }

    /**
     * Set watchdog check interval
     */
    PipelineConfig& with_watchdog_interval(std::chrono::seconds interval) {
        watchdog_interval = interval;
        return *this;
    }

    /**
     * Set long-running task warning threshold
     */
    PipelineConfig& with_warning_threshold(std::chrono::seconds threshold) {
        long_task_warning_threshold = threshold;
        return *this;
    }

    /**
     * Set executor idle timeout
     */
    PipelineConfig& with_executor_idle_timeout(std::chrono::seconds timeout) {
        executor_idle_timeout = timeout;
        return *this;
    }

    /**
     * Set executor deadlock timeout
     */
    PipelineConfig& with_executor_deadlock_timeout(
        std::chrono::seconds timeout) {
        executor_deadlock_timeout = timeout;
        return *this;
    }

    /**
     * Create sequential execution configuration (1 thread)
     */
    static PipelineConfig sequential() {
        return PipelineConfig().with_executor_threads(1).with_watchdog(
            false);  // Less useful for single-threaded
    }

    /**
     * Create parallel execution configuration
     */
    static PipelineConfig parallel(std::size_t num_threads = 0) {
        return PipelineConfig()
            .with_executor_threads(num_threads)  // 0 = hardware_concurrency
            .with_watchdog(true);
    }

    /**
     * Create default configuration
     */
    static PipelineConfig default_config() {
        PipelineConfig config;
        config.executor_threads = 0;  // hardware_concurrency
        config.scheduler_threads = 1;
        config.enable_watchdog = true;
        config.global_timeout = std::chrono::seconds(0);
        config.default_task_timeout = std::chrono::seconds(0);
        config.watchdog_interval = std::chrono::seconds(1);
        config.long_task_warning_threshold = std::chrono::seconds(300);
        config.executor_idle_timeout = std::chrono::seconds(300);
        config.executor_deadlock_timeout = std::chrono::seconds(600);
        return config;
    }

    /**
     * Create configuration with timeouts
     */
    static PipelineConfig with_timeouts(
        std::size_t num_threads = 0,
        std::chrono::seconds global_timeout = std::chrono::seconds(60),
        std::chrono::seconds task_timeout = std::chrono::seconds(30)) {
        return PipelineConfig()
            .with_executor_threads(num_threads)
            .with_watchdog(true)
            .with_global_timeout(global_timeout)
            .with_task_timeout(task_timeout);
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_CONFIG_H
