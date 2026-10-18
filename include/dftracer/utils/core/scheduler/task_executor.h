#ifndef DFTRACER_UTILS_CORE_SCHEDULER_TASK_EXECUTOR_H
#define DFTRACER_UTILS_CORE_SCHEDULER_TASK_EXECUTOR_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/executor/executor.h>
#include <dftracer/utils/core/io/io_backend.h>

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
class Scheduler;

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
    std::size_t io_pool_size = 0;  ///< 0 = hardware_concurrency
    io::IoBackendType io_backend_type = io::IoBackendType::Auto;
    unsigned io_batch_threshold = 16;
};

struct TaskInfo {
    TaskIndex task_id;
    TaskIndex parent_task_id;  ///< -1 for root tasks
    std::string name;
    std::size_t worker_id;

    enum State {
        Queued,     ///< In queue (shared or local)
        Running,    ///< Currently executing
        Waiting,    ///< Waiting for child tasks
        Completed,  ///< Successfully finished
        Failed      ///< Failed with error
    } state;

    std::chrono::steady_clock::time_point queued_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;

    std::vector<TaskIndex> child_task_ids;
    std::atomic<std::size_t> completed_children{0};

    std::string error_message;

    enum Location { SharedQueue, LocalQueue, Executing, Done } location;
};

struct TaskProgress {
    TaskIndex task_id;
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
        std::optional<TaskIndex> current_task_id;
        std::string current_task_name;
        std::size_t local_queue_depth;
    };
    std::vector<WorkerStatus> workers;

    /// (task_id, error_msg)
    std::vector<std::pair<TaskIndex, std::string>> recent_errors;
};

/**
 * TaskExecutor - an Executor that also runs DAG tasks and owns its lifecycle.
 *
 * What Pipeline, Scheduler and Watchdog require. Implement this to run the
 * pipeline on something other than the bundled thread pool.
 */
class TaskExecutor : public Executor {
   public:
    using CompletionCallback = std::function<void(std::shared_ptr<Task>)>;

    virtual void start() = 0;
    virtual void shutdown() = 0;
    virtual void request_shutdown() = 0;

    virtual void set_completion_callback(CompletionCallback callback) = 0;
    virtual void set_scheduler(Scheduler* scheduler) = 0;

    virtual void submit_task(std::shared_ptr<Task> task,
                             std::shared_ptr<std::any> input,
                             TaskIndex parent_task_id = -1) = 0;
    virtual TaskIndex enqueue_tracked(
        coro::Coro coro, std::string name,
        std::shared_ptr<std::atomic<TaskIndex>> tid_out = nullptr) = 0;

    /// Task the calling worker is running, -1 when not on a worker thread.
    virtual TaskIndex current_worker_task_id() const = 0;

    virtual bool is_responsive() const = 0;
    virtual ExecutorProgress get_progress() const = 0;
};

/// Build the executor described by `config`. The implementation is chosen
/// here so callers never name a concrete executor type.
std::unique_ptr<TaskExecutor> make_task_executor(const ExecutorConfig& config);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_SCHEDULER_TASK_EXECUTOR_H
