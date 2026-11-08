// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_SCHEDULER_THREAD_POOL_EXECUTOR_H
#define DFTRACER_UTILS_RUNTIME_SCHEDULER_THREAD_POOL_EXECUTOR_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/scheduler/run_queues.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace dftracer::utils {

/// Executes queued coroutine work on a pool of worker threads. Pool size
/// defaults to hardware_concurrency.
///
/// Scheduling design after the Go runtime scheduler,
/// https://github.com/golang/go (BSD-3-Clause), file src/runtime/proc.go: the
/// slot for the task woken last, the global queue check every 61 ticks, and a
/// wake only when no worker is searching. Also after the Tokio multi-thread
/// scheduler, https://github.com/tokio-rs/tokio (MIT): the inject queue and
/// the limit of searching workers to half the pool.
class ThreadPoolExecutor final : public TaskExecutor {
   private:
    // The run queues of one worker. A retired worker's Core stays in the steal
    // list and goes to the next worker, so a thief never reads freed memory.
    struct alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) Core {
        // The task this worker woke last, run next. Only the owner and the
        // sysmon take it; thieves leave it, so a wake stays on the warm core.
        alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) std::atomic<void*> lifo{
            nullptr};
        // Sysmon only: the slot's value at its last look.
        void* lifo_seen = nullptr;
        // Owner only.
        std::uint32_t tick = 0;
        std::uint32_t lifo_streak = 0;
        std::uint32_t steal_seed = 0;
        runtime::scheduler::LocalQueue queue;
    };

    // Aligned to avoid false sharing between adjacent workers.
    struct alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) WorkerContext {
        std::size_t worker_id;
        ThreadPoolExecutor* owner = nullptr;
        Core* core = nullptr;

        // Counted in searching_. Touched only by this worker's thread, except
        // that a waker sets it for a parked worker through notified.
        bool searching{false};

        // Guarded by sleepers_mutex_. A waker sets it and counts this worker
        // in searching_ when it takes it from sleepers_.
        bool notified{false};
        std::condition_variable park_cv;
        // Guarded by sleepers_mutex_. Set while this worker parks in the I/O
        // backend, which park_cv does not wake.
        void* park_token{nullptr};

        // Health monitoring for watchdog
        std::atomic<bool> is_idle{false};
        std::atomic<uint64_t> tasks_executed{0};
        // Run-queue entries this worker resumed. Written only by its own
        // thread, and is_responsive() sums them to see progress.
        std::atomic<std::uint64_t> resumes{0};
        std::chrono::steady_clock::time_point last_activity;

        // Current task info (for debugging/watchdog)
        std::atomic<common::TaskIndex> current_task_id{-1};
        dstd::String current_task_name;
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
        // Regions open on this worker. Touched only by its own thread.
        int blocking_depth{0};
        std::atomic<std::int64_t> blocking_since_ns{0};

        dstd::Thread thread;

        explicit WorkerContext(std::size_t id) : worker_id(id) {}
    };

    // Per-worker contexts. Mutated (add/retire) and iterated (progress,
    // shutdown) under workers_mutex_; the hot enqueue/wake path is futex-based
    // and never touches this vector.
    dstd::Vector<dstd::UniquePtr<WorkerContext>> workers_;
    mutable std::mutex workers_mutex_;
    std::atomic<std::size_t> live_workers_{0};    // current live worker threads
    std::atomic<std::size_t> idle_workers_{0};    // currently parked in wait()
    std::atomic<std::size_t> next_worker_id_{0};  // monotonic ids, churn-safe

    std::atomic<bool> running_{false};
    std::atomic<std::size_t> thread_count_{0};    // the running cap
    // Elastic floor; == thread_count_ when eager.
    std::atomic<std::size_t> min_workers_{0};
    // The full pool up front (true) or grown on demand from the floor. Kept
    // apart from the floor, which is also the cap of an elastic pool of one.
    std::atomic<bool> eager_{true};
    // Live-thread ceiling for blocking-handoff replacements; the permit gate
    // still bounds RUNNING threads. A backstop, not a working bound.
    std::atomic<std::size_t> max_live_{0};
    // Serializes set_threads, grow_io_threads and start.
    std::mutex resize_mutex_;

    static constexpr int BLK_NONE = 0;
    static constexpr int BLK_SYSCALL = 1;
    static constexpr int BLK_HANDED_OFF = 2;
    // Blocks shorter than this pay no handoff; longer ones the sysmon reclaims.
    static constexpr std::int64_t BLOCK_HANDOFF_NS = 100000;
    // Headroom above thread_count_ for blocking-handoff replacement threads. A
    // burst of concurrent block_on calls parks that many workers at once
    // and each needs a replacement runner, so a small ceiling (e.g. one per
    // core) deadlocks when the inner scopes starve; the permit gate still
    // bounds actually-running threads.
    static constexpr std::size_t MAX_BLOCKING_REPLACEMENTS = 256;

    // Available run permits.
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT)
        std::atomic<std::ptrdiff_t> permits_{0};
    // Workers currently parked inside block_on.
    std::atomic<std::size_t> blocked_workers_{0};
    // Workers parked in acquire_permit waiting for a slot.
    std::atomic<std::size_t> permit_waiters_{0};
    // Reclaims permits from long blocks; parks on sysmon_cv_ when none
    // blocking.
    dstd::Thread sysmon_thread_;
    std::mutex sysmon_mutex_;
    std::condition_variable sysmon_cv_;
    // Elastic grow/shrink coordinator (started only when min_workers_ <
    // thread_count_).
    dstd::Thread monitor_thread_;
    std::mutex coord_mutex_;
    std::condition_variable coord_cv_;
    bool grow_pending_{false};

    // Workers parked in drive_until. Their LIFO slots are empty.
    std::atomic<std::size_t> parked_drivers_{0};
    // Sysmon asleep with every worker parked and nothing blocking.
    std::atomic<bool> sysmon_parked_{false};
    // Tasks the sysmon took from a LIFO slot and has not queued yet.
    std::atomic<std::size_t> lifo_in_transit_{0};

    // Global tracking, padded to avoid false sharing between counters.
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> tasks_completed_{0};
    std::atomic<std::size_t> tasks_failed_{0};
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> total_tasks_submitted_{0};

    // Resume total and time of the last is_responsive() call that saw it
    // change.
    mutable std::atomic<std::uint64_t> seen_resumes_{0};
    mutable std::atomic<std::int64_t> last_progress_ns_;

    // Responsiveness timeout thresholds
    std::chrono::seconds idle_timeout_;
    std::atomic<std::chrono::seconds::rep> deadlock_timeout_s_;
    // Elastic idle-retire delay (monitor_loop). Longer = warmer pool.
    std::chrono::milliseconds keepalive_;
    coro::TimerService timer_service_;

    // Task registry for progress tracking. A Map, because a flat map relocates
    // its elements and TaskInfo holds an atomic.
    dstd::Map<common::TaskIndex, TaskInfo> task_registry_;
    mutable std::shared_mutex registry_mutex_;

    // Finished tasks, oldest first, and how many stay in task_registry_.
    // Both are guarded by registry_mutex_.
    dstd::Deque<common::TaskIndex> finished_;
    std::size_t task_history_;
    void retire_locked(common::TaskIndex id);

    // Completion hooks of tracked coros, one per live tracked id, erased when
    // the task completes. A Map keeps an element's address stable across
    // inserts.
    dstd::Map<common::TaskIndex, coro::detail::TaskTracking> trackings_;

    // Counter for tracked-coro IDs (negative to avoid collision with DAG IDs).
    std::atomic<common::TaskIndex> next_coro_task_id_{-1000000};

    // Every Core ever made, read by thieves without a lock: load the count
    // first, then the list, which is at least that long. Appended to under
    // workers_mutex_; a full list is copied to a larger one and stays alive
    // until shutdown for a thief still reading it.
    std::atomic<std::size_t> core_count_{0};
    std::atomic<Core**> core_list_{nullptr};
    dstd::Vector<dstd::Vector<Core*>> core_lists_;
    dstd::Vector<dstd::UniquePtr<Core>> cores_;
    dstd::Vector<Core*> free_cores_;

    runtime::scheduler::InjectQueue inject_;
    // Workers looking for work. A wake is skipped while one is, and the last
    // one to find work wakes the next.
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> searching_{0};
    // Enqueues from threads outside this pool still running. The handle they
    // queue can finish and let the owner destroy the executor before the
    // enqueue returns, so the destructor waits for this to reach zero.
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT)
        std::atomic<std::size_t> foreign_enqueues_{0};

    // Parked workers, most recent last, so a wake finds warm caches. The park
    // publishes itself here, then checks the queues again, and a post publishes
    // its task, then reads idle_workers_. Each side has a seq_cst fence between
    // the two, so at least one sees the other and no wake is lost. The timeout
    // only lets a worker trim its heap.
    std::mutex sleepers_mutex_;
    dstd::Vector<WorkerContext*> sleepers_;

    // The inject queue goes first every so many tasks, so tasks there do not
    // starve behind a busy worker. The value is the one in Go.
    static constexpr std::uint32_t INJECT_CHECK_INTERVAL = 61;
    static constexpr std::uint32_t LIFO_STREAK_MAX = 3;
    static constexpr std::size_t INJECT_BATCH_MAX = 128;
    static constexpr int STEAL_ROUNDS = 4;
    static constexpr std::size_t CORE_LIST_MIN = 64;
    // How often the sysmon looks for a LIFO task behind a long computation.
    static constexpr std::int64_t LIFO_CHECK_NS = 1000000;
    static constexpr std::chrono::milliseconds IDLE_PARK_MIN{1};
    static constexpr std::chrono::milliseconds IDLE_PARK_MAX{256};

    // I/O backend (owned by executor, created by factory)
    dstd::UniquePtr<runtime::io::IoBackend> io_backend_;

    // Waiters that only another party can finish, and the token scopes watch.
    coro::detail::ParkRegistry park_registry_;
    dstd::SharedPtr<std::atomic<bool>> shutdown_token_ =
        dstd::make_shared<std::atomic<bool>>(false);

    // Configuration (stored from ExecutorConfig)
    std::atomic<std::size_t> io_pool_size_{0};
    runtime::io::IoBackendType io_backend_type_ =
        runtime::io::IoBackendType::Auto;
    unsigned io_batch_threshold_ = 16;
    dstd::UniquePtr<runtime::io::IoBackend> (*io_backend_factory_)(
        void* context) = nullptr;
    void* io_backend_context_ = nullptr;

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

    // Spawns one worker up to the thread_count_ cap.
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

    coro::TimerService* timers() noexcept override { return &timer_service_; }

    bool is_running() const override { return running_.load(); }
    void quiesce();

    coro::detail::ParkNode* park(
        void* waiter, coro::detail::ParkCancelFn cancel) noexcept override {
        return park_registry_.park(waiter, cancel);
    }
    void unpark(coro::detail::ParkNode* n) noexcept override {
        park_registry_.unpark(n);
    }
    void wake_cancelled() noexcept override {
        park_registry_.cancel_requested_waiters();
    }
    dstd::SharedPtr<std::atomic<bool>> shutdown_token() const override {
        return shutdown_token_;
    }

    std::size_t thread_count() const override {
        return thread_count_.load(std::memory_order_acquire);
    }

    void set_threads(std::size_t threads, dstd::Optional<bool> eager) override;
    void grow_io_threads(std::size_t threads) override;
    void set_deadlock_timeout(std::chrono::seconds timeout) override;

    /// Live worker threads right now (floats under thread_count_ once the
    /// elastic policy is driving add_worker/retire_worker); thread_count_ is
    /// the cap.
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

    std::size_t io_pool_size() const override {
        return io_pool_size_.load(std::memory_order_acquire);
    }

    bool has_io_backend() const noexcept override {
        return io_backend_ != nullptr;
    }

    /// Must check has_io_backend() first.
    runtime::io::IoBackend& io_backend() override { return *io_backend_; }
    const runtime::io::IoBackend& io_backend() const { return *io_backend_; }

    /// Whether the executor is making progress; false if it appears hung.
    /// Used by the watchdog.
    bool is_responsive() const override;

    ExecutorProgress progress() const override;

    /// From a worker of this pool the task runs next on that worker. From any
    /// other thread it goes to the inject queue and wakes a worker.
    void post(std::coroutine_handle<> handle) noexcept override;

    /// Queues behind the other tasks of the calling worker.
    void post_yield(std::coroutine_handle<> handle) noexcept override;

    /// Enqueue a Coro with progress tracking in task_registry_.
    common::TaskIndex enqueue_tracked(
        coro::detail::Coro coro, dstd::StringView name,
        dstd::SharedPtr<std::atomic<common::TaskIndex>> tid_out =
            nullptr) override;

    void mark_coro_started(common::TaskIndex id) override;
    void mark_coro_completed(common::TaskIndex id) override;
    void mark_coro_failed(common::TaskIndex id,
                          dstd::StringView message) override;

    common::TaskIndex current_worker_task_id() const override;

    bool drive_until(dstd::FunctionRef<bool()> done) override;

   private:
    void worker_thread(WorkerContext* context);

    /// Cancels pending I/O and every parked waiter, and runs the woken frames
    /// until each reaches its final suspend. Gives up with a warning when a
    /// waiter stays parked without progress.
    void cancel_in_flight();

    /**
     * Elastic policy loop (runs only when min_workers_ < thread_count_): grow
     * one worker per tick while there is run-queue backlog under the cap;
     * retire one per keep-alive of sustained idle down to min_workers_.
     */
    void monitor_loop();

    TaskProgress task_progress_of(const TaskInfo& info) const;

    /// The calling thread's context when it is a worker of this pool.
    WorkerContext* owned_context() const noexcept;

    void schedule_foreign(void* task) noexcept;

    /// A task became stealable: wake a parked worker unless one is searching.
    /// The caller has published the task.
    void notify() noexcept;
    /// Takes one parked worker and counts it as searching. Without @p force it
    /// gives up when a worker is searching already.
    bool wake_sleeper(bool force) noexcept;

    Core* acquire_core();

    /// The next task of @p core's own queues and the inject queue, or null.
    void* next_task(Core& core);
    void* take_injected(Core& core);
    /// Tries every other core, then the inject queue.
    void* steal_task(Core& core);
    bool try_start_searching(WorkerContext* ctx) noexcept;
    void stop_searching(WorkerContext* ctx) noexcept;
    /// Moves the LIFO task to the local queue, where thieves can reach it.
    void flush_lifo(Core& core);
    /// Moves everything in @p core to the inject queue for a worker that exits.
    void flush_core(Core& core);
    /// Tasks in every queue, the LIFO slots too when @p with_lifo is set. May
    /// be stale.
    std::size_t pending_tasks(bool with_lifo) const noexcept;
    /// Parks until woken, retired or stopped.
    void reserve_sleepers();
    void park_worker(WorkerContext* ctx);
    /// A LIFO task whose worker stayed in one resume for a whole tick goes to
    /// the inject queue, for a worker that can run it.
    void check_lifo_stalls() noexcept;
    void wake_sysmon_if_parked();

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

    void wake_all_workers();

    /// The completion hook a tracked coro's promise calls.
    static void on_tracked_task_complete(void* owner,
                                         common::TaskIndex id) noexcept;

    friend struct coro::detail::CoroPromise;
};

void* current_worker_context();
void set_current_worker_context(void* context);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_SCHEDULER_THREAD_POOL_EXECUTOR_H
