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
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/backend_type.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>

namespace dftracer::utils {

namespace io {
class IoBackend;
}  // namespace io

class Task;

/// Who submitted a task to a @ref TaskExecutor and hears when it ends. Several
/// owners share one executor.
class TaskOwner {
   public:
    /// Called once per submitted task, from the worker that ends it. The
    /// executor does not touch the owner after this returns.
    virtual void finish_task(dstd::SharedPtr<Task> task) noexcept = 0;
    /// True once the owner stopped: a task of it that has not started does
    /// not run.
    virtual bool is_stopping() const noexcept = 0;

   protected:
    ~TaskOwner() = default;
};

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
    io::IoBackendType io_backend_type = io::IoBackendType::Auto;
    unsigned io_batch_threshold = 16;
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
    common::TaskIndex parent_task_id;  ///< -1 for root tasks
    dstd::String name;
    std::size_t worker_id;

    TaskState state;

    std::chrono::steady_clock::time_point queued_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;

    dstd::Vector<common::TaskIndex> child_task_ids;
    std::atomic<std::size_t> completed_children{0};

    dstd::String error_message;

    TaskLocation location;
};

struct TaskProgress {
    common::TaskIndex task_id;
    dstd::String name;

    double queued_duration_ms;
    double execution_duration_ms;

    std::size_t total_subtasks;
    std::size_t completed_subtasks;
    double progress_percentage;  ///< 0-100

    dstd::Vector<TaskProgress> children;

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
 * TaskExecutor - a @ref coro::Executor that also runs DAG tasks and owns its
 * lifecycle, and the one executor shape that names an I/O backend.
 *
 * What Pipeline, Scheduler and Watchdog require. Implement this to run the
 * pipeline on something other than the bundled thread pool.
 */
class TaskExecutor : public coro::Executor {
   public:
    virtual void start() = 0;
    virtual void shutdown() = 0;

    /// Queues @p handle as untracked work: the @ref coro::Executor::post of
    /// this executor shape. A task the runtime tracks goes through
    /// @ref enqueue with its task id. A final subclass overrides it to skip
    /// the second virtual call.
    void post(std::coroutine_handle<> handle) noexcept override {
        enqueue(handle, -1);
    }

    /// Primary submission path; all lightweight work funnels through here.
    /// Work: a queue push and an atomic signal. task_id is a tracked-task id
    /// for registry progress, or -1 if untracked.
    virtual void enqueue(std::coroutine_handle<> handle,
                         common::TaskIndex task_id = -1) = 0;

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

    /// Runs `task` and then calls `owner->finish_task(task)`, once, from the
    /// worker that ends it, so `owner` must outlive the task. A task whose
    /// owner is stopping before it starts does not run. It ends with an
    /// Interrupted error, as does a queued task that shutdown() destroys.
    virtual void submit_task(dstd::SharedPtr<Task> task,
                             dstd::SharedPtr<std::any> input, TaskOwner* owner,
                             common::TaskIndex parent_task_id = -1) = 0;
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
    virtual io::IoBackend& io_backend() = 0;
    virtual std::size_t io_pool_size() const = 0;

    /// Records `executor` as the I/O backend owner of the calling thread, or
    /// clears it with null. A worker calls it where it sets
    /// @ref Executor::current, so a scope that suppresses that executor
    /// falls back to blocking calls too. Not inherited by threads the executor
    /// did not start.
    static void set_current_backend(TaskExecutor* executor) noexcept;
};

/// Marks the tracked task @p id as failed with the what() of @p error, or
/// "Unknown exception" for an error that is not a std::exception.
void mark_coro_failed(TaskExecutor& executor, common::TaskIndex id,
                      std::exception_ptr error) noexcept;

/// Build the executor described by `config`. The implementation is chosen
/// here so callers never name a concrete executor type.
dstd::UniquePtr<TaskExecutor> make_task_executor(const ExecutorConfig& config);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H
