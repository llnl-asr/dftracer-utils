#ifndef DFTRACER_UTILS_CORE_PIPELINE_THREAD_POOL_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_THREAD_POOL_EXECUTOR_H

#include <concurrentqueue.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/common/timer_service.h>
#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/executor.h>

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils {

class Task;
class Scheduler;

/// Executes queued coroutine work on a pool of worker threads, notifying the
/// scheduler on completion. Pool size defaults to hardware_concurrency.
class ThreadPoolExecutor : public TaskExecutor {
   public:
    using TaskExecutor::CompletionCallback;

   private:
    // Aligned to avoid false sharing between adjacent workers.
    struct alignas(DFTRACER_OPTIMAL_ALIGNMENT) WorkerContext {
        std::size_t worker_id;

        // Health monitoring for watchdog
        std::atomic<bool> is_idle{false};
        std::atomic<uint64_t> tasks_executed{0};
        std::chrono::steady_clock::time_point last_activity;

        // Current task info (for debugging/watchdog)
        std::atomic<TaskIndex> current_task_id{-1};
        std::string current_task_name;
        std::mutex task_name_mutex;

        // Set to ask this worker to exit its loop (dynamic retire); checked
        // only at the loop top, so retire is always between tasks, never
        // mid-resume.
        std::atomic<bool> retire{false};

        // Held stickily across resumes; released only when the worker parks
        // idle. Touched only by this worker's own thread, so unsynced.
        bool holds_permit{false};

        // Lazy handoff state (BLK_*). Worker and sysmon race a CAS out of
        // SYSCALL: exactly one of keep-on-exit and reclaim wins.
        std::atomic<int> blk_state{0};
        std::atomic<std::int64_t> blocking_since_ns{0};

        std::thread thread;

        explicit WorkerContext(std::size_t id) : worker_id(id) {}
    };

    // Per-worker contexts. Mutated (add/retire) and iterated (progress,
    // shutdown) under workers_mutex_; the hot enqueue/wake path is futex-based
    // and never touches this vector.
    std::vector<std::unique_ptr<WorkerContext>> workers_;
    mutable std::mutex workers_mutex_;
    std::atomic<std::size_t> live_workers_{0};    // current live worker threads
    std::atomic<std::size_t> idle_workers_{0};    // currently parked in wait()
    std::atomic<std::size_t> next_worker_id_{0};  // monotonic ids, churn-safe

    std::atomic<bool> running_{false};
    std::size_t num_threads_;                     // the running cap
    std::size_t min_workers_;  // elastic floor; == num_threads_ means eager
    // Live-thread ceiling for blocking-handoff replacements; the permit gate
    // still bounds RUNNING threads. A backstop, not a working bound.
    std::size_t max_live_;

    static constexpr int BLK_NONE = 0;
    static constexpr int BLK_SYSCALL = 1;
    static constexpr int BLK_HANDED_OFF = 2;
    // Blocks shorter than this pay no handoff; longer ones the sysmon reclaims.
    static constexpr std::int64_t BLOCK_HANDOFF_NS = 100000;
    // Headroom above num_threads_ for blocking-handoff replacement threads. A
    // burst of concurrent run_blocking calls parks that many workers at once
    // and each needs a replacement runner, so a small ceiling (e.g. one per
    // core) deadlocks when the inner scopes starve; the permit gate still
    // bounds actually-running threads.
    static constexpr std::size_t MAX_BLOCKING_REPLACEMENTS = 256;

    // Available run permits.
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) std::atomic<std::ptrdiff_t> permits_{0};
    // Workers currently parked inside run_blocking.
    std::atomic<std::size_t> blocked_workers_{0};
    // Workers parked in acquire_permit waiting for a slot.
    std::atomic<std::size_t> permit_waiters_{0};
    // Reclaims permits from long blocks; parks on sysmon_cv_ when none
    // blocking.
    std::thread sysmon_thread_;
    std::mutex sysmon_mutex_;
    std::condition_variable sysmon_cv_;
    // Elastic grow/shrink coordinator (started only when min_workers_ <
    // num_threads_).
    std::thread monitor_thread_;
    std::mutex coord_mutex_;
    std::condition_variable coord_cv_;
    bool grow_pending_{false};

    CompletionCallback completion_callback_;

    // Reference to scheduler for dynamic task context
    Scheduler* scheduler_{nullptr};

    // Global tracking, padded to avoid false sharing between counters
    // and with work_signal_ below.
    alignas(DFTRACER_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> tasks_completed_{0};
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) std::atomic<std::size_t> tasks_started_{
        0};
    alignas(DFTRACER_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> total_tasks_submitted_{0};

    std::atomic<std::int64_t> last_activity_ns_;

    std::atomic<bool> shutdown_requested_{false};

    // Responsiveness timeout thresholds
    std::chrono::seconds idle_timeout_;
    std::chrono::seconds deadlock_timeout_;
    // Elastic idle-retire delay (monitor_loop). Longer = warmer pool.
    std::chrono::milliseconds keepalive_;
    TimerService timer_service_;

    // Task registry for progress tracking
    std::unordered_map<TaskIndex, TaskInfo> task_registry_;
    mutable std::shared_mutex registry_mutex_;

    // Counter for tracked-coro IDs (negative to avoid collision with DAG IDs).
    std::atomic<TaskIndex> next_coro_task_id_{-1000000};

    // Run queue entry. -1 means untracked.
    struct RunQueueEntry {
        std::coroutine_handle<> handle{};
        TaskIndex task_id{-1};
        long long monitor_id{-1};  // coroutine monitor id, -1 if untracked
    };

    moodycamel::ConcurrentQueue<RunQueueEntry> run_queue_;
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) std::atomic<std::uint64_t> work_signal_{
        0};

    // Idle park. run_queue_ is a moodycamel::ConcurrentQueue whose try_dequeue
    // can report empty while another producer's item is still in flight, even
    // after the worker observed that producer's work_signal_ bump, so a wakeup
    // can be missed with the item already queued. The timeout is therefore
    // load-bearing, not an optimization: it re-checks the queue so a missed
    // wakeup self-heals. Do not replace wait_for with an untimed wait. Closing
    // the race instead would need work_signal_ bumped under idle_mutex_ on the
    // hot enqueue path, or a semaphore-backed queue.
    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
    static constexpr std::chrono::milliseconds IDLE_PARK_MIN{1};
    static constexpr std::chrono::milliseconds IDLE_PARK_MAX{256};

    // Deferred destruction queue for released Coro handles.
    // FinalAwaiter pushes here; worker loop drains periodically.
    moodycamel::ConcurrentQueue<std::coroutine_handle<>> destroy_queue_;

    // I/O backend (owned by executor, created by factory)
    std::unique_ptr<io::IoBackend> io_backend_;

    // Configuration (stored from ExecutorConfig)
    std::size_t io_pool_size_ = 0;
    io::IoBackendType io_backend_type_ = io::IoBackendType::AUTO;
    unsigned io_batch_threshold_ = 16;

   public:
    explicit ThreadPoolExecutor(const ExecutorConfig& config = {});

    ~ThreadPoolExecutor() override;

    ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
    ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

    // Prevent moving (threads are not movable once started)
    ThreadPoolExecutor(ThreadPoolExecutor&&) = delete;
    ThreadPoolExecutor& operator=(ThreadPoolExecutor&&) = delete;

    void start() override;

    void shutdown() override;

    // Spawns one worker up to the num_threads_ cap.
    void add_worker();
    // Asks one worker to exit and joins it. Safe while
    /// running; a retired worker strands no work (the run queue is shared) and
    /// exits only between tasks
    void retire_worker();

    /// Blocking handoff. enter_blocking() marks the block and keeps the permit;
    /// only if the block outlasts BLOCK_HANDOFF_NS does the sysmon reclaim it
    /// and wake/spawn a replacement. exit_blocking() reclaims the permit with a
    /// CAS on a short block, or re-acquires one (parking until free) if it was
    /// handed off.
    void enter_blocking() override;
    void exit_blocking() override;

    void set_completion_callback(CompletionCallback callback) override;

    void set_scheduler(Scheduler* scheduler) override {
        scheduler_ = scheduler;
    }

    TimerService& get_timer_service() override { return timer_service_; }

    bool is_running() const override { return running_.load(); }

    std::size_t get_num_threads() const override { return num_threads_; }

    /// Live worker threads right now (floats under num_threads_ once the
    /// elastic policy is driving add_worker/retire_worker); num_threads_ is the
    /// cap.
    std::size_t live_workers() const {
        return live_workers_.load(std::memory_order_acquire);
    }

    /// Available run permits and workers currently in a blocking handoff.
    /// Observability for tests: at quiescence permits == cap and blocked == 0.
    std::ptrdiff_t available_permits() const {
        return permits_.load(std::memory_order_acquire);
    }
    std::size_t blocked_workers() const {
        return blocked_workers_.load(std::memory_order_acquire);
    }

    std::size_t get_io_pool_size() const override { return io_pool_size_; }

    bool has_io_backend() const noexcept override {
        return io_backend_ != nullptr;
    }

    /// Must check has_io_backend() first.
    io::IoBackend& io_backend() override { return *io_backend_; }
    const io::IoBackend& io_backend() const { return *io_backend_; }

    /**
     * Request graceful shutdown
     * Stops accepting new tasks and waits for current tasks to complete
     */
    void request_shutdown() override;

    bool is_shutdown_requested() const { return shutdown_requested_.load(); }

    /// Whether the executor is making progress; false if it appears hung.
    /// Used by the watchdog.
    bool is_responsive() const override;

    ExecutorProgress get_progress() const override;

    /// Resume a coroutine handle on the pool. For combinators (when_all etc.)
    /// that must resume from a completion callback without calling .resume().
    void schedule_coroutine_resumption(std::coroutine_handle<> handle) override;

    /// Primary submission path; all lightweight work funnels through here.
    /// Cost: ~20ns (lock-free queue push + atomic signal). task_id is a
    /// tracked-task id for registry progress, or -1 if untracked.
    void enqueue(std::coroutine_handle<> handle,
                 TaskIndex task_id = -1) override;

    /// Enqueue a Coro with progress tracking in task_registry_.
    TaskIndex enqueue_tracked(
        coro::Coro coro, std::string name,
        std::shared_ptr<std::atomic<TaskIndex>> tid_out = nullptr) override;

    void mark_coro_completed(TaskIndex id) override;

    /// Submit a DAG task for execution. parent_task_id tracks the parent, or
    /// -1 for a root task.
    void submit_task(std::shared_ptr<Task> task,
                     std::shared_ptr<std::any> input,
                     TaskIndex parent_task_id = -1) override;

    TaskIndex current_worker_task_id() const override;

    void drive_until(const std::function<bool()>& done) override;

    /// Schedule a completed Coro handle for deferred destruction. Called from
    /// CoroPromise::FinalAwaiter for released handles.
    void schedule_destroy(std::coroutine_handle<> handle) override;

   private:
    void worker_thread(WorkerContext* context);

    /**
     * Elastic policy loop (runs only when min_workers_ < num_threads_): grow
     * one worker per tick while there is run-queue backlog under the cap;
     * retire one per keep-alive of sustained idle down to min_workers_.
     */
    void monitor_loop();

    TaskProgress build_task_progress_tree(
        TaskIndex task_id, std::unordered_set<TaskIndex>& processed) const;

    void notify_completion(std::shared_ptr<Task> task);

    void mark_activity();

    void signal_global_work();

    /// Acquire a run permit for `ctx`, parking on the permit futex until one is
    /// free. Returns false without a permit if the worker should exit (shutdown
    /// or retire observed while waiting).
    bool acquire_permit(WorkerContext* ctx);
    /// Release `ctx`'s permit if it holds one, notifying a permit-waiter.
    void release_permit(WorkerContext* ctx);
    /// A permit was just freed for a blocking handoff: wake a parked worker or
    /// spawn a replacement (up to max_live_) so the freed slot gets used.
    void ensure_running_worker();
    /// Reclaims permits from workers blocking past BLOCK_HANDOFF_NS.
    void sysmon_loop();
    /// Spawn one worker if live < `cap`. Returns true if spawned. Takes
    /// workers_mutex_; best_effort uses try_lock and gives up on contention.
    bool spawn_worker(std::size_t cap, bool best_effort = false);

    void wake_one_worker();

    void wake_all_workers();

    /// Create a Coro that executes a Task with full bookkeeping. The returned
    /// Coro is at initial_suspend: the caller must set the executor on the
    /// promise and enqueue via release().
    coro::Coro run_task(std::shared_ptr<Task> task,
                        std::shared_ptr<std::any> input);

    void drain_destroy_queue();

    friend class Scheduler;
    friend struct coro::CoroPromise;
};

void* get_current_worker_context();

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_THREAD_POOL_EXECUTOR_H
