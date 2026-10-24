#ifndef DFTRACER_UTILS_RUNTIME_PIPELINE_PIPELINE_CONFIG_H
#define DFTRACER_UTILS_RUNTIME_PIPELINE_PIPELINE_CONFIG_H

#include <dftracer/utils/runtime/io/io_backend.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dftracer::utils {

class Task;

/**
 * Error handling policy for pipeline execution
 */
enum class ErrorPolicy {
    FailFast,  ///< Stop immediately on first error (default)
    Continue,  ///< Continue other branches, skip children of failed tasks
    Custom     ///< User-provided handler
};

/**
 * Error handler function type
 * Parameters: (failed_task, exception_ptr)
 */
using ErrorHandler =
    std::function<void(std::shared_ptr<Task>, std::exception_ptr)>;

/**
 * Configuration for Pipeline execution.
 *
 * Thread Architecture:
 * - Executor threads: Worker pool that executes task functions (compute
 * threads)
 * - Watchdog: Optional monitoring thread for hang detection
 *
 * Usage (Fluent API):
 *   auto config = PipelineConfig()
 *       .with_name("MyPipeline")
 *       .with_compute_threads(16)
 *       .with_error_policy(ErrorPolicy::FAIL_FAST)
 *       .with_watchdog(true)
 *       .with_global_timeout(std::chrono::seconds(30))
 *       .with_task_timeout(std::chrono::seconds(10));
 */
struct PipelineConfig {
    std::string name = "";
    std::size_t executor_threads =
        0;  ///< 0 = hardware_concurrency (compute threads)
    /// Elastic floor: workers to start with, growing toward executor_threads on
    /// backlog and idle-retiring back down. 0 = eager (full pool up front, no
    /// resizing). Unset = resolve from the DFTRACER_UTILS_ELASTIC env default
    /// at construction; an explicit value
    /// (with_eager/with_elastic/with_min_workers) always wins, so a test can
    /// pin either path regardless of the env.
    std::optional<std::size_t> min_workers;
    std::chrono::milliseconds elastic_keepalive{250};
    ErrorPolicy error_policy = ErrorPolicy::FailFast;
    ErrorHandler error_handler =
        nullptr;                  ///< Custom error handler (for CUSTOM policy)
    bool enable_watchdog = true;  ///< Hang detection
    std::chrono::seconds global_timeout{0};        ///< 0 = wait forever
    std::chrono::seconds default_task_timeout{0};  ///< 0 = wait forever
    std::chrono::seconds watchdog_interval{1};
    std::chrono::seconds long_task_warning_threshold{
        300};     ///< Warning threshold (5 minutes)
    std::chrono::seconds executor_idle_timeout{
        300};     ///< Executor idle timeout (5 minutes)
    std::chrono::seconds executor_deadlock_timeout{
        600};     ///< Executor deadlock timeout (10 minutes)
    std::chrono::microseconds timeslice_duration{
        10'000};  ///< Coroutine yield timeslice (10ms, 0 = disabled)
    std::size_t io_thread_count = 0;   ///< 0 = hardware_concurrency
    io::IoBackendType io_backend_type = io::IoBackendType::Auto;
    unsigned io_batch_threshold = 16;  ///< SQE batch threshold (0 = per-op)

    PipelineConfig& with_name(std::string pipeline_name) {
        name = std::move(pipeline_name);
        return *this;
    }

    /**
     * Set number of compute threads (worker threads for CPU-bound work)
     * 0 = hardware_concurrency (default)
     */
    PipelineConfig& with_compute_threads(std::size_t threads) {
        executor_threads = threads;
        return *this;
    }

    /**
     * Pin the elastic floor (workers to start with, growing to
     * executor_threads on demand). 0 = eager. Overrides the env default.
     */
    PipelineConfig& with_min_workers(std::size_t workers) {
        min_workers = workers;
        return *this;
    }

    /**
     * Pin an elastic pool: start at one worker and grow on demand. Overrides
     * the env default, so a test can exercise the elastic path
     * deterministically.
     */
    PipelineConfig& with_elastic() {
        min_workers = 1;
        return *this;
    }

    /**
     * Pin an eager pool: full pool up front, never resized. Overrides the env
     * default. For a latency-sensitive service, or to test the eager path.
     */
    PipelineConfig& with_eager() {
        min_workers = 0;
        return *this;
    }

    PipelineConfig& with_error_policy(ErrorPolicy policy) {
        error_policy = policy;
        return *this;
    }

    /**
     * Set custom error handler (automatically sets policy to CUSTOM)
     */
    PipelineConfig& with_error_handler(ErrorHandler handler) {
        error_handler = std::move(handler);
        error_policy = ErrorPolicy::Custom;
        return *this;
    }

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

    PipelineConfig& with_watchdog_interval(std::chrono::seconds interval) {
        watchdog_interval = interval;
        return *this;
    }

    PipelineConfig& with_warning_threshold(std::chrono::seconds threshold) {
        long_task_warning_threshold = threshold;
        return *this;
    }

    PipelineConfig& with_executor_idle_timeout(std::chrono::seconds timeout) {
        executor_idle_timeout = timeout;
        return *this;
    }

    PipelineConfig& with_executor_deadlock_timeout(
        std::chrono::seconds timeout) {
        executor_deadlock_timeout = timeout;
        return *this;
    }

    /**
     * Set I/O thread pool size (used by thread pool and epoll backends)
     */
    PipelineConfig& with_io_threads(std::size_t count) {
        io_thread_count = count;
        return *this;
    }

    /**
     * Force a specific I/O backend (AUTO = runtime detection)
     */
    PipelineConfig& with_io_backend(io::IoBackendType type) {
        io_backend_type = type;
        return *this;
    }

    /**
     * Set I/O batch threshold for batched SQE submission.
     * When pending ops reach this count, they are flushed in one
     * syscall. 0 = per-op submission (no batching).
     */
    PipelineConfig& with_io_batch_size(unsigned threshold) {
        io_batch_threshold = threshold;
        return *this;
    }

    /**
     * Create sequential execution configuration (1 thread)
     */
    static PipelineConfig sequential() {
        return PipelineConfig().with_compute_threads(1).with_watchdog(
            false);  // Less useful for single-threaded
    }

    /**
     * Create parallel execution configuration
     */
    static PipelineConfig parallel(std::size_t num_threads = 0) {
        return PipelineConfig()
            .with_compute_threads(num_threads)  // 0 = hardware_concurrency
            .with_watchdog(true);
    }

    static PipelineConfig default_config() {
        PipelineConfig config;
        config.executor_threads = 0;  // hardware_concurrency
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
            .with_compute_threads(num_threads)
            .with_watchdog(true)
            .with_global_timeout(global_timeout)
            .with_task_timeout(task_timeout);
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_PIPELINE_PIPELINE_CONFIG_H
