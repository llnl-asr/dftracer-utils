// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H
#define DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/backend_type.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>

namespace dftracer::utils {

namespace runtime::io {
class IoBackend;
}  // namespace runtime::io

struct ExecutorConfig {
    std::size_t thread_count =
        0;  ///< 0 = hardware_concurrency; the RUNNING cap
    /// Elastic floor. 0 = thread_count (eager: spawn the full pool up front, no
    /// resizing). Set 1..thread_count to run an elastic pool: start at this
    /// many workers, grow toward thread_count on backlog, idle-retire back to
    /// it - so a mostly-idle runtime does not hold hardware_concurrency threads
    /// (matters on shared HPC login nodes).
    std::size_t min_workers = 0;
    /// Elastic keep-alive: an idle worker above min_workers is retired only
    /// after it has been idle this long.
    std::chrono::milliseconds elastic_keepalive{250};
    std::chrono::seconds idle_timeout{5};
    std::chrono::seconds deadlock_timeout{10};
    std::size_t io_pool_size = 0;  ///< 0 = one per worker
    runtime::io::IoBackendType io_backend_type =
        runtime::io::IoBackendType::Auto;
    unsigned io_batch_threshold = 16;
    /// Makes the I/O backend in place of io_backend_type, once for each start
    /// of the executor, which owns the result and starts it. It gets
    /// io_backend_context, which must stay valid as long as the executor runs.
    /// Must not return null.
    dstd::UniquePtr<runtime::io::IoBackend> (*io_backend_factory)(
        void* context) = nullptr;
    void* io_backend_context = nullptr;
    /// Finished tasks kept for progress. The oldest are dropped first.
    std::size_t task_history = common::constants::TASK_HISTORY_LIMIT;
};

enum class TaskState : std::uint8_t {
    Queued,
    Running,
    Waiting,  ///< Waiting for child tasks
    Completed,
    Failed
};

enum class TaskLocation : std::uint8_t {
    SharedQueue,
    LocalQueue,
    Executing,
    Done
};

/// Lower-case name of @p state: "queued", "running", "waiting", "completed"
/// or "failed".
dstd::StringView task_state_name(TaskState state) noexcept;

struct TaskInfo {
    common::TaskIndex task_id;
    dstd::String name;
    std::size_t worker_id;

    TaskState state;

    std::chrono::steady_clock::time_point queued_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;

    dstd::String error_message;

    TaskLocation location;
};

struct TaskProgress {
    common::TaskIndex task_id;
    dstd::String name;

    double queued_duration_ms;
    double execution_duration_ms;

    double progress_percentage;  ///< 0-100

    std::int32_t worker;  ///< Worker of a LocalQueue or Executing task, else -1
    TaskState state;
    TaskLocation location;
};

struct ExecutorProgress {
    std::size_t total_tasks_submitted;
    std::size_t tasks_queued;
    std::size_t tasks_running;
    /// Tasks that ended without an error. A failed task counts only in
    /// @ref tasks_failed.
    std::size_t tasks_completed;
    std::size_t tasks_failed;

    dstd::Vector<std::size_t> worker_queue_depths;

    dstd::Vector<TaskProgress> root_tasks;

    struct WorkerStatus {
        std::size_t worker_id;
        bool is_idle;
        dstd::Optional<common::TaskIndex> current_task_id;
        dstd::String current_task_name;
        std::size_t local_queue_depth;
    };
    dstd::Vector<WorkerStatus> workers;

    /// (task_id, error_msg)
    dstd::Vector<dstd::Pair<common::TaskIndex, dstd::String>> recent_errors;
};

/**
 * TaskExecutor - a @ref coro::Executor that owns its lifecycle, and the one
 * executor shape that names an I/O backend.
 *
 * What @ref Runtime requires. Implement this to run tasks on something other
 * than the bundled thread pool.
 */
class TaskExecutor : public coro::Executor {
   public:
    /// Fails when the executor cannot get what it needs to run, such as an
    /// I/O backend.
    virtual dstd::Result<void> start() = 0;
    virtual void shutdown() = 0;

    /// Records tracked task @p id as running on the calling worker. Called
    /// once, when the task first runs.
    virtual void mark_coro_started(common::TaskIndex id) = 0;
    /// Records tracked task @p id as finished. Called once, from the worker
    /// that ends it.
    virtual void mark_coro_completed(common::TaskIndex id) = 0;
    /// Marks the tracked task @p id as failed with @p message. A later
    /// mark_coro_completed for the same task leaves it failed.
    virtual void mark_coro_failed(common::TaskIndex id,
                                  dstd::StringView message) = 0;

    virtual bool is_running() const = 0;
    virtual std::size_t thread_count() const = 0;
    std::size_t concurrency() const noexcept override { return thread_count(); }

    /// Runs this executor's work on the calling thread until `done` holds;
    /// false when this executor cannot lend the calling thread. See
    /// @ref coro::Executor::drive_until.
    bool drive_until(dstd::FunctionRef<bool()> done) override = 0;

    virtual common::TaskIndex enqueue_tracked(
        coro::detail::Coro coro, dstd::StringView name,
        dstd::SharedPtr<std::atomic<common::TaskIndex>> tid_out = nullptr) = 0;

    /// Task the calling worker is running, -1 when not on a worker thread.
    virtual common::TaskIndex current_worker_task_id() const = 0;

    /// Moves the running cap to `threads` (0 = hardware_concurrency) while work
    /// runs. A smaller cap lets fewer tasks start until running ones finish.
    /// `eager` true spawns the full pool, false grows it on demand from one
    /// worker, unset keeps the mode. Thread safe.
    virtual void set_threads(std::size_t threads,
                             dstd::Optional<bool> eager) = 0;
    /// Raises the I/O threads to `threads`. A smaller count changes nothing.
    virtual void grow_io_threads(std::size_t threads) = 0;
    /// The stall after which is_responsive() reports a deadlock.
    virtual void set_deadlock_timeout(std::chrono::seconds timeout) = 0;

    virtual bool is_responsive() const = 0;
    virtual ExecutorProgress progress() const = 0;

    /// Without a backend the io layer falls back to blocking syscalls.
    virtual bool has_io_backend() const noexcept = 0;
    /// Must check @ref has_io_backend first.
    virtual runtime::io::IoBackend& io_backend() = 0;
    virtual std::size_t io_pool_size() const = 0;

    /// Records `executor` as the I/O backend owner of the calling thread, or
    /// clears it with null. A worker calls it where it sets
    /// @ref Executor::current, so a scope that suppresses that executor
    /// falls back to blocking calls too. Not inherited by threads the executor
    /// did not start.
    static void set_current_backend(TaskExecutor* executor) noexcept;
};

/// Marks the tracked task @p id as failed with the text of @p error.
void mark_coro_failed(TaskExecutor& executor, common::TaskIndex id,
                      const dstd::Error& error) noexcept;

/// Build the executor described by `config`. The implementation is chosen
/// here so callers never name a concrete executor type.
dstd::UniquePtr<TaskExecutor> make_task_executor(const ExecutorConfig& config);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H
