// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H
#define DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/io/io_backend.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils {

class Task;

/// Who submitted a task to a @ref TaskExecutor and hears when it ends. Several
/// owners share one executor.
class TaskOwner {
   public:
    /// Called once per submitted task, from the worker that ends it. The
    /// executor does not touch the owner after this returns.
    virtual void finish_task(std::shared_ptr<Task> task) noexcept = 0;
    /// True once the owner stopped: a task of it that has not started does
    /// not run.
    virtual bool is_stopping() const noexcept = 0;

   protected:
    ~TaskOwner() = default;
};

struct ExecutorConfig {
    std::size_t num_threads = 0;  ///< 0 = hardware_concurrency; the RUNNING cap
    /// Elastic floor. 0 = num_threads (eager: spawn the full pool up front, no
    /// resizing). Set 1..num_threads to run an elastic pool: start at this many
    /// workers, grow toward num_threads on backlog, idle-retire back to it - so
    /// a mostly-idle runtime does not hold hardware_concurrency threads
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
};

struct TaskInfo {
    common::TaskIndex task_id;
    common::TaskIndex parent_task_id;  ///< -1 for root tasks
    std::string name;
    std::size_t worker_id;

    enum State {
        Queued,
        Running,
        Waiting,  ///< Waiting for child tasks
        Completed,
        Failed
    } state;

    std::chrono::steady_clock::time_point queued_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;

    std::vector<common::TaskIndex> child_task_ids;
    std::atomic<std::size_t> completed_children{0};

    std::string error_message;

    enum Location { SharedQueue, LocalQueue, Executing, Done } location;
};

struct TaskProgress {
    common::TaskIndex task_id;
    std::string name;
    std::string
        state;  ///< "queued", "running", "waiting", "completed", "failed"

    double queued_duration_ms;
    double execution_duration_ms;

    std::size_t total_subtasks;
    std::size_t completed_subtasks;
    double progress_percentage;  ///< 0-100

    /// "shared_queue", "worker_2_local", "executing_on_worker_3"
    std::string location;

    std::vector<TaskProgress> children;
};

struct ExecutorProgress {
    std::size_t total_tasks_submitted;
    std::size_t tasks_queued;
    std::size_t tasks_running;
    std::size_t tasks_completed;
    std::size_t tasks_failed;

    std::vector<std::size_t> worker_queue_depths;

    std::vector<TaskProgress> root_tasks;

    struct WorkerStatus {
        std::size_t worker_id;
        bool is_idle;
        std::optional<common::TaskIndex> current_task_id;
        std::string current_task_name;
        std::size_t local_queue_depth;
    };
    std::vector<WorkerStatus> workers;

    /// (task_id, error_msg)
    std::vector<std::pair<common::TaskIndex, std::string>> recent_errors;
};

/**
 * TaskExecutor - an Executor that also runs DAG tasks and owns its lifecycle.
 *
 * What Pipeline, Scheduler and Watchdog require. Implement this to run the
 * pipeline on something other than the bundled thread pool.
 */
class TaskExecutor : public Executor {
   public:
    virtual void start() = 0;
    virtual void shutdown() = 0;

    /// Runs `task` and then calls `owner->finish_task(task)`, once, from the
    /// worker that ends it, so `owner` must outlive the task. A task whose
    /// owner is stopping before it starts does not run. It ends with an
    /// Interrupted error, as does a queued task that shutdown() destroys.
    virtual void submit_task(std::shared_ptr<Task> task,
                             std::shared_ptr<std::any> input, TaskOwner* owner,
                             common::TaskIndex parent_task_id = -1) = 0;
    virtual common::TaskIndex enqueue_tracked(
        coro::Coro coro, std::string name,
        std::shared_ptr<std::atomic<common::TaskIndex>> tid_out = nullptr) = 0;

    /// Task the calling worker is running, -1 when not on a worker thread.
    virtual common::TaskIndex current_worker_task_id() const = 0;

    /// Moves the running cap to `threads` (0 = hardware_concurrency) while work
    /// runs. A smaller cap lets fewer tasks start until running ones finish.
    /// `eager` true spawns the full pool, false grows it on demand from one
    /// worker, unset keeps the mode. Thread safe.
    virtual void set_threads(std::size_t threads,
                             std::optional<bool> eager) = 0;
    /// Raises the I/O threads to `threads`. A smaller count changes nothing.
    virtual void grow_io_threads(std::size_t threads) = 0;
    /// The stall after which is_responsive() reports a deadlock.
    virtual void set_deadlock_timeout(std::chrono::seconds timeout) = 0;

    virtual bool is_responsive() const = 0;
    virtual ExecutorProgress get_progress() const = 0;
};

/// Build the executor described by `config`. The implementation is chosen
/// here so callers never name a concrete executor type.
std::unique_ptr<TaskExecutor> make_task_executor(const ExecutorConfig& config);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_SCHEDULER_TASK_EXECUTOR_H
