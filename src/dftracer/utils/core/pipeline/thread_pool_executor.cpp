#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/io/io_backend_factory.h>
#include <dftracer/utils/core/pipeline/thread_pool_executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/monitor.h>
#include <zlib.h>

#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace dftracer::utils {

namespace {

// Force zlib-ng's lazy CPU-feature functable init single-threaded before any
// worker spawns, so concurrent first-use does not race on the global table.
void warmup_vendored_libs() noexcept {
    unsigned char src[64];
    for (std::size_t i = 0; i < sizeof(src); ++i) {
        src[i] = static_cast<unsigned char>(i);
    }
    unsigned char comp[128];
    unsigned char back[64];

    z_stream def{};
    if (deflateInit2(&def, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8,
                     Z_DEFAULT_STRATEGY) == Z_OK) {
        def.next_in = src;
        def.avail_in = sizeof(src);
        def.next_out = comp;
        def.avail_out = sizeof(comp);
        deflate(&def, Z_FINISH);
        uInt comp_len = static_cast<uInt>(sizeof(comp) - def.avail_out);
        deflateEnd(&def);

        z_stream inf{};
        if (inflateInit2(&inf, 31) == Z_OK) {
            inf.next_in = comp;
            inf.avail_in = comp_len;
            inf.next_out = back;
            inf.avail_out = sizeof(back);
            inflate(&inf, Z_FINISH);
            inflateEnd(&inf);
        }
    }
}

}  // namespace

ThreadPoolExecutor::ThreadPoolExecutor(const ExecutorConfig& config)
    : num_threads_(config.num_threads == 0 ? hardware_concurrency()
                                           : config.num_threads),
      last_activity_ns_(
          std::chrono::steady_clock::now().time_since_epoch().count()),
      idle_timeout_(config.idle_timeout),
      deadlock_timeout_(config.deadlock_timeout),
      keepalive_(config.elastic_keepalive),
      io_pool_size_(config.io_pool_size == 0 ? hardware_concurrency()
                                             : config.io_pool_size),
      io_backend_type_(config.io_backend_type),
      io_batch_threshold_(config.io_batch_threshold) {
    if (num_threads_ == 0) {
        num_threads_ = 2;
    }
    if (io_pool_size_ == 0) {
        io_pool_size_ = 2;
    }
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    // Per-Executor thread churn dominates runtime when Valgrind serializes and
    // instruments every thread, so cap the pools.
    if (num_threads_ > 2) num_threads_ = 2;
    if (io_pool_size_ > 2) io_pool_size_ = 2;
#endif
    min_workers_ =
        (config.min_workers == 0 || config.min_workers > num_threads_)
            ? num_threads_
            : config.min_workers;
    max_live_ = num_threads_ + MAX_BLOCKING_REPLACEMENTS;
    DFTRACER_UTILS_LOG_DEBUG(
        "Executor created with %zu threads, idle_timeout=%lld s, "
        "deadlock_timeout=%lld s",
        num_threads_, static_cast<long long>(idle_timeout_.count()),
        static_cast<long long>(deadlock_timeout_.count()));
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    shutdown();
    drain_destroy_queue();
}

void ThreadPoolExecutor::start() {
    if (running_) {
        DFTRACER_UTILS_LOG_WARN("%s", "Executor already running");
        return;
    }

    running_ = true;
    workers_.clear();
    workers_.reserve(num_threads_);
    permits_.store(static_cast<std::ptrdiff_t>(num_threads_),
                   std::memory_order_release);

    static std::once_flag warmup_once;
    std::call_once(warmup_once, warmup_vendored_libs);

    timer_service_.start();

    // Create and start I/O backend before workers so workers
    // can use it immediately.
    io_backend_ = io::create_io_backend(*this, io_pool_size_, io_backend_type_,
                                        io_batch_threshold_);
    io_backend_->start();

    // Spawn the floor (== the cap in eager mode). The elastic monitor grows the
    // rest on demand. Contexts first so workers_ is stable before any worker
    // thread can iterate it.
    for (std::size_t i = 0; i < min_workers_; ++i) {
        auto worker = std::make_unique<WorkerContext>(
            next_worker_id_.fetch_add(1, std::memory_order_relaxed));
        worker->last_activity = std::chrono::steady_clock::now();
        workers_.push_back(std::move(worker));
    }
    live_workers_.store(workers_.size(), std::memory_order_release);

    // Start worker threads after all contexts are in place.
    for (auto& worker : workers_) {
        worker->thread =
            std::thread(&ThreadPoolExecutor::worker_thread, this, worker.get());
    }

    // Elastic mode only: a monitor drives grow/shrink between the floor and
    // cap.
    if (min_workers_ < num_threads_) {
        monitor_thread_ = std::thread(&ThreadPoolExecutor::monitor_loop, this);
    }

    // Reclaims permits from long blocks; blocking can occur in any mode.
    sysmon_thread_ = std::thread(&ThreadPoolExecutor::sysmon_loop, this);

    DFTRACER_UTILS_LOG_DEBUG(
        "Executor started with %zu worker threads (cap %zu)", min_workers_,
        num_threads_);
}

void ThreadPoolExecutor::shutdown() {
    if (!running_) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutting down executor");
    running_ = false;
    wake_all_workers();

    coord_cv_.notify_all();
    if (monitor_thread_.joinable()) monitor_thread_.join();

    {
        std::lock_guard<std::mutex> lock(sysmon_mutex_);
        sysmon_cv_.notify_all();
    }
    if (sysmon_thread_.joinable()) sysmon_thread_.join();

    std::lock_guard<std::mutex> workers_lock(workers_mutex_);

    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    if (io_backend_) {
        io_backend_->stop();
        io_backend_.reset();
    }

    drain_destroy_queue();
    {
        RunQueueEntry orphan;
        while (run_queue_.try_dequeue(orphan)) {
            if (orphan.handle) {
                orphan.handle.destroy();
            }
        }
    }

    workers_.clear();
    live_workers_.store(0, std::memory_order_release);
    timer_service_.stop();

    // Drain the main thread's thread-local destroy list (for
    // coroutines whose FinalAwaiter ran on the main thread).
    drain_thread_local_destroys();

    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor shutdown complete");
}

bool ThreadPoolExecutor::spawn_worker(std::size_t cap, bool best_effort) {
    std::unique_lock<std::mutex> lock(workers_mutex_, std::defer_lock);
    if (best_effort) {
        if (!lock.try_lock()) return false;
    } else {
        lock.lock();
    }
    if (!running_.load(std::memory_order_acquire)) return false;
    if (live_workers_.load(std::memory_order_relaxed) >= cap) return false;
    auto worker = std::make_unique<WorkerContext>(
        next_worker_id_.fetch_add(1, std::memory_order_relaxed));
    worker->last_activity = std::chrono::steady_clock::now();
    WorkerContext* ptr = worker.get();
    workers_.push_back(std::move(worker));
    live_workers_.fetch_add(1, std::memory_order_acq_rel);
    ptr->thread = std::thread(&ThreadPoolExecutor::worker_thread, this, ptr);
    return true;
}

void ThreadPoolExecutor::add_worker() {
    spawn_worker(num_threads_, /*best_effort=*/false);
}

void ThreadPoolExecutor::retire_worker() {
    std::unique_ptr<WorkerContext> victim;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        if (workers_.empty()) return;
        // Pull the target out of the vector under the lock, so wake/progress no
        // longer see it, then drive its exit outside the lock.
        victim = std::move(workers_.back());
        workers_.pop_back();
        live_workers_.fetch_sub(1, std::memory_order_acq_rel);
    }
    victim->retire.store(true, std::memory_order_release);
    wake_all_workers();  // wake it if parked so it observes retire
    if (victim->thread.joinable()) victim->thread.join();
    // victim (and its WorkerContext) is destroyed here, after the join.
}

bool ThreadPoolExecutor::acquire_permit(WorkerContext* ctx) {
    std::ptrdiff_t c = permits_.load(std::memory_order_acquire);
    for (;;) {
        while (c > 0) {
            if (permits_.compare_exchange_weak(c, c - 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return true;
            }
        }
        if (!running_.load(std::memory_order_acquire) ||
            ctx->retire.load(std::memory_order_acquire)) {
            return false;
        }
        permit_waiters_.fetch_add(1, std::memory_order_acq_rel);
        permits_.wait(0, std::memory_order_acquire);
        permit_waiters_.fetch_sub(1, std::memory_order_acq_rel);
        c = permits_.load(std::memory_order_acquire);
    }
}

void ThreadPoolExecutor::release_permit(WorkerContext* ctx) {
    if (!ctx->holds_permit) return;
    ctx->holds_permit = false;
    permits_.fetch_add(1, std::memory_order_acq_rel);
    permits_.notify_one();
}

void ThreadPoolExecutor::ensure_running_worker() {
    // A permit-waiter will take the freed slot; no need to spawn.
    if (permit_waiters_.load(std::memory_order_acquire) > 0) return;
    if (idle_workers_.load(std::memory_order_acquire) > 0) {
        wake_one_worker();
        return;
    }
    const std::size_t justified =
        num_threads_ + blocked_workers_.load(std::memory_order_acquire);
    if (live_workers_.load(std::memory_order_acquire) < justified) {
        spawn_worker(max_live_, /*best_effort=*/true);
    }
}

void ThreadPoolExecutor::enter_blocking() {
    auto* ctx = static_cast<WorkerContext*>(get_current_worker_context());
    if (!ctx || !ctx->holds_permit) return;
    ctx->blocking_since_ns.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_relaxed);
    ctx->blk_state.store(BLK_SYSCALL, std::memory_order_release);
    // Only the 0->1 transition can find the sysmon parked; skip the wake else.
    // Notify under sysmon_mutex_ so the wake cannot slip between the sysmon's
    // predicate check and its wait().
    if (blocked_workers_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        std::lock_guard<std::mutex> lock(sysmon_mutex_);
        sysmon_cv_.notify_one();
    }
}

void ThreadPoolExecutor::exit_blocking() {
    auto* ctx = static_cast<WorkerContext*>(get_current_worker_context());
    if (!ctx) return;
    blocked_workers_.fetch_sub(1, std::memory_order_acq_rel);
    int expected = BLK_SYSCALL;
    if (ctx->blk_state.compare_exchange_strong(expected, BLK_NONE,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return;  // kept our permit
    }
    // Handed off: reacquire a permit before resuming.
    ctx->holds_permit = false;
    ctx->blk_state.store(BLK_NONE, std::memory_order_release);
    if (acquire_permit(ctx)) ctx->holds_permit = true;
}

void ThreadPoolExecutor::sysmon_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(sysmon_mutex_);
            if (blocked_workers_.load(std::memory_order_acquire) == 0) {
                // Park until a worker enters a blocking region or we shut down;
                // both notify under sysmon_mutex_, so there is no wakeup to
                // miss and no need to poll.
                sysmon_cv_.wait(lock, [this] {
                    return blocked_workers_.load(std::memory_order_acquire) >
                               0 ||
                           !running_.load(std::memory_order_acquire);
                });
            }
        }
        if (!running_.load(std::memory_order_acquire)) break;
        if (blocked_workers_.load(std::memory_order_acquire) == 0) continue;

        const std::int64_t now =
            std::chrono::steady_clock::now().time_since_epoch().count();
        int handed = 0;
        {
            std::unique_lock<std::mutex> wl(workers_mutex_, std::try_to_lock);
            if (wl.owns_lock()) {
                for (auto& w : workers_) {
                    if (w->blk_state.load(std::memory_order_acquire) !=
                        BLK_SYSCALL)
                        continue;
                    if (now - w->blocking_since_ns.load(
                                  std::memory_order_acquire) <
                        BLOCK_HANDOFF_NS)
                        continue;
                    int expected = BLK_SYSCALL;
                    if (w->blk_state.compare_exchange_strong(
                            expected, BLK_HANDED_OFF, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        permits_.fetch_add(1, std::memory_order_acq_rel);
                        permits_.notify_one();
                        ++handed;
                    }
                }
            }
        }
        for (int i = 0; i < handed; ++i) ensure_running_worker();
        std::this_thread::sleep_for(std::chrono::nanoseconds(BLOCK_HANDOFF_NS));
    }
}

void ThreadPoolExecutor::monitor_loop() {
    using clock = std::chrono::steady_clock;
    const auto keepalive = keepalive_;
    auto last_busy = clock::now();

    std::unique_lock<std::mutex> lock(coord_mutex_);
    while (running_.load(std::memory_order_acquire)) {
        // Park until enqueue signals unmet demand (grow) or a keep-alive
        // elapses (shrink check). No busy-poll.
        coord_cv_.wait_for(lock, keepalive, [this] {
            return grow_pending_ || !running_.load(std::memory_order_acquire);
        });
        grow_pending_ = false;
        lock.unlock();  // add/retire take workers_mutex_, not coord_mutex_
        if (!running_.load(std::memory_order_acquire)) {
            lock.lock();
            break;
        }

        if (run_queue_.size_approx() > 0 ||
            idle_workers_.load(std::memory_order_acquire) <
                live_workers_.load(std::memory_order_acquire)) {
            last_busy = clock::now();
        }
        // Grow: spawn until the backlog is covered or we hit the cap - all at
        // once, so a burst reaches full parallelism in one wake.
        while (run_queue_.size_approx() > 0 &&
               live_workers_.load(std::memory_order_acquire) < num_threads_) {
            add_worker();
        }
        // Shrink: one worker per keep-alive of sustained idle, down to the
        // floor (gentle, so a brief lull does not thrash the pool).
        if (live_workers_.load(std::memory_order_acquire) > min_workers_ &&
            clock::now() - last_busy >= keepalive) {
            retire_worker();
            last_busy = clock::now();
        }
        lock.lock();
    }
}

void ThreadPoolExecutor::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

void ThreadPoolExecutor::worker_thread(WorkerContext* context) {
    DFTRACER_UTILS_LOG_DEBUG("Worker %zu started", context->worker_id);

    set_current_worker_context(context);
    Executor::set_current(this);
    coro::reset_timeslice();

    // Grows while idle so the pool is not polling, resets on work found.
    std::chrono::milliseconds idle_park = IDLE_PARK_MIN;

    // Fixed for the process lifetime; read once to keep the resume loop cheap.
    const bool monitor = utilities::monitoring_enabled();
    if (monitor) {
        utilities::monitor_set_worker(static_cast<int>(context->worker_id));
    }

    while (running_ && !context->retire.load(std::memory_order_acquire)) {
        // Hold a permit to run; a surplus thread (live > cap) parks for a slot.
        if (!context->holds_permit) {
            if (!acquire_permit(context)) break;
            context->holds_permit = true;
        }

        RunQueueEntry pending_entry;

        const std::uint64_t observed_signal =
            work_signal_.load(std::memory_order_acquire);

        // Run queue: coroutine handles from enqueue() and
        // schedule_coroutine_resumption().
        if (run_queue_.try_dequeue(pending_entry)) {
            idle_park = IDLE_PARK_MIN;
            coro::reset_timeslice();
            context->is_idle.store(false, std::memory_order_relaxed);
            std::coroutine_handle<> pending_resume = pending_entry.handle;
            if (pending_resume && !pending_resume.done()) {
                // Id comes from the entry, not the promise: foreign promise
                // types have no task_id field.
                TaskIndex tid = pending_entry.task_id;
                if (tid >= 0) {
                    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                    auto it = task_registry_.find(tid);
                    if (it != task_registry_.end() &&
                        it->second.state == TaskInfo::QUEUED) {
                        it->second.state = TaskInfo::RUNNING;
                        it->second.started_at =
                            std::chrono::steady_clock::now();
                        it->second.worker_id = context->worker_id;
                        it->second.location = TaskInfo::EXECUTING;
                        ++tasks_started_;
                    }
                }
                DFTRACER_TSAN_ACQUIRE(pending_resume.address());
                if (monitor) {
                    utilities::monitor_resume_begin(pending_entry.monitor_id);
                }
                pending_resume.resume();
                if (monitor) {
                    utilities::monitor_resume_end(pending_resume.address(),
                                                  pending_resume.done());
                }
            }
            // Destroy coroutine frames that FinalAwaiter deferred to this
            // thread.  Safe: resume() has fully returned, so the frame
            // is suspended at final_suspend and no code references it.
            drain_thread_local_destroys();
            drain_destroy_queue();
        }
        // No work available -- sleep until signaled.
        else {
            context->is_idle.store(true, std::memory_order_relaxed);
            // Flush any batched I/O operations before sleeping.
            if (io_backend_) {
                io_backend_->flush();
            }
            // Opportunistic I/O polling before sleeping
            if (io_backend_) {
                auto reaped = io_backend_->poll(0);
                if (reaped > 0) {
                    drain_destroy_queue();
                    continue;
                }
            }
            drain_destroy_queue();
            if (!running_.load(std::memory_order_acquire) ||
                context->retire.load(std::memory_order_acquire)) {
                break;
            }
            // Give up the permit for a waiting/surplus thread, then park.
            release_permit(context);
            idle_workers_.fetch_add(1, std::memory_order_acq_rel);
            {
                std::unique_lock<std::mutex> lock(idle_mutex_);
                if (work_signal_.load(std::memory_order_acquire) !=
                        observed_signal ||
                    idle_cv_.wait_for(lock, idle_park) !=
                        std::cv_status::timeout)
                    idle_park = IDLE_PARK_MIN;
                else if (idle_park < IDLE_PARK_MAX)
                    idle_park *= 2;
            }
            idle_workers_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Release the permit if we broke out of the loop still holding it.
    release_permit(context);

    // Final drain: destroy any frames deferred during the last resume().
    drain_thread_local_destroys();

    Executor::set_current(nullptr);
    set_current_worker_context(nullptr);

    DFTRACER_UTILS_LOG_DEBUG("Worker %zu terminated", context->worker_id);
}

void ThreadPoolExecutor::drive_until(const std::function<bool()>& done) {
    constexpr int IDLE_POLL_MS = 1;

    while (!done()) {
        const std::uint64_t observed_signal =
            work_signal_.load(std::memory_order_acquire);

        RunQueueEntry entry;
        if (run_queue_.try_dequeue(entry)) {
            coro::reset_timeslice();
            if (entry.handle && !entry.handle.done()) {
                DFTRACER_TSAN_ACQUIRE(entry.handle.address());
                entry.handle.resume();
            }
            drain_thread_local_destroys();
            drain_destroy_queue();
            continue;
        }

        drain_destroy_queue();
        if (done()) break;
        if (!running_.load(std::memory_order_acquire)) break;

        if (io_backend_) {
            io_backend_->flush();
            io_backend_->poll(IDLE_POLL_MS);
            continue;
        }
        // Bounded for the same reason as worker_thread's park: a run_queue_
        // enqueue can be missed with the item already queued, so re-check.
        std::unique_lock<std::mutex> lock(idle_mutex_);
        if (work_signal_.load(std::memory_order_acquire) == observed_signal)
            idle_cv_.wait_for(lock, IDLE_PARK_MIN);
    }
    drain_thread_local_destroys();
}

void ThreadPoolExecutor::notify_completion(std::shared_ptr<Task> task) {
    if (completion_callback_) completion_callback_(task);
}

void ThreadPoolExecutor::request_shutdown() {
    if (shutdown_requested_.load()) {
        return;  // Already requested
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutdown requested for executor");
    shutdown_requested_ = true;
}

void ThreadPoolExecutor::schedule_coroutine_resumption(
    std::coroutine_handle<> handle) {
    // Delegate to enqueue() -- unified path for all coroutine handles.
    enqueue(handle);
}

void ThreadPoolExecutor::signal_global_work() {
    work_signal_.fetch_add(1, std::memory_order_acq_rel);
    wake_one_worker();
    if (min_workers_ < num_threads_ &&
        idle_workers_.load(std::memory_order_acquire) == 0 &&
        live_workers_.load(std::memory_order_relaxed) < num_threads_) {
        {
            std::lock_guard<std::mutex> lock(coord_mutex_);
            grow_pending_ = true;
        }
        coord_cv_.notify_one();
    }
}

void ThreadPoolExecutor::wake_one_worker() {
    work_signal_.notify_one();
    idle_cv_.notify_one();
}

void ThreadPoolExecutor::wake_all_workers() {
    work_signal_.fetch_add(1, std::memory_order_release);
    work_signal_.notify_all();
    idle_cv_.notify_all();
    // Wake workers parked in acquire_permit so they re-check running_/retire.
    permits_.notify_all();
}

// Helper function for when_all.h (avoids circular dependency)

void ThreadPoolExecutor::enqueue(std::coroutine_handle<> handle,
                                 TaskIndex task_id) {
    if (!handle || handle.done()) {
        return;  // Invalid or already completed
    }

    long long monitor_id = -1;
    if (utilities::monitoring_enabled()) {
        // task_id >= 0 marks a tracked submitted task; otherwise it is spawn
        // fan-out (or a re-enqueue of an already-seen coroutine).
        monitor_id = utilities::monitor_enqueue(
            handle.address(), task_id >= 0 ? utilities::CoroKind::Task
                                           : utilities::CoroKind::Spawn);
    }
    DFTRACER_TSAN_RELEASE(handle.address());
    run_queue_.enqueue(RunQueueEntry{handle, task_id, monitor_id});
    signal_global_work();
}

bool ThreadPoolExecutor::is_responsive() const {
    // If shutdown was requested, consider unresponsive
    if (shutdown_requested_.load()) {
        return false;
    }

    // If not running, not responsive
    if (!running_.load()) {
        return false;
    }

    // Check if all threads might be deadlocked
    // (all threads busy but no progress for a while)
    std::size_t started = tasks_started_.load();
    std::size_t completed = tasks_completed_.load();
    std::size_t active = started - completed;

    if (active >= num_threads_) {
        // All threads busy - check if making progress
        auto now = std::chrono::steady_clock::now();
        auto last_ns = last_activity_ns_.load(std::memory_order_acquire);
        auto last_tp = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(last_ns));
        auto idle_time = now - last_tp;

        // If all threads busy but no activity for deadlock_timeout,
        // likely deadlocked
        if (idle_time > deadlock_timeout_) {
            DFTRACER_UTILS_LOG_WARN(
                "Executor appears deadlocked: %zu threads, %zu active "
                "tasks, idle for %lld ms",
                num_threads_, active,
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        idle_time)
                        .count()));
            return false;
        }
    }

    return true;
}

void ThreadPoolExecutor::mark_activity() {
    last_activity_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_release);
}

ExecutorProgress ThreadPoolExecutor::get_progress() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    ExecutorProgress progress;

    // Overall stats
    progress.total_tasks_submitted = total_tasks_submitted_.load();
    progress.tasks_completed = tasks_completed_.load();

    // Count task states
    progress.tasks_queued = 0;
    progress.tasks_running = 0;
    progress.tasks_failed = 0;

    for (const auto& [task_id, info] : task_registry_) {
        switch (info.state) {
            case TaskInfo::QUEUED:
                progress.tasks_queued++;
                break;
            case TaskInfo::RUNNING:
            case TaskInfo::WAITING:
                progress.tasks_running++;
                break;
            case TaskInfo::COMPLETED:
                // Already counted
                break;
            case TaskInfo::FAILED:
                progress.tasks_failed++;
                break;
        }

        if (info.state == TaskInfo::FAILED && !info.error_message.empty()) {
            progress.recent_errors.push_back({task_id, info.error_message});
        }
    }

    for (std::size_t i = 0; i < workers_.size(); ++i) {
        // No per-worker local queues anymore; report 0.
        progress.worker_queue_depths.push_back(0);
    }

    // Build task trees (find root tasks)
    std::unordered_set<TaskIndex> processed;
    for (const auto& [task_id, info] : task_registry_) {
        if (info.parent_task_id == -1) {  // Root task
            auto task_progress = build_task_progress_tree(task_id, processed);
            progress.root_tasks.push_back(task_progress);
        }
    }

    // Worker states (dynamic set; lock against add/retire mutation)
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        for (const auto& worker : workers_) {
            ExecutorProgress::WorkerStatus status;
            status.worker_id = worker->worker_id;
            status.is_idle = worker->is_idle.load();

            TaskIndex current_id = worker->current_task_id.load();
            if (current_id != -1) {
                status.current_task_id = current_id;
                std::lock_guard<std::mutex> name_lock(worker->task_name_mutex);
                status.current_task_name = worker->current_task_name;
            }

            status.local_queue_depth = 0;

            progress.workers.push_back(status);
        }
    }

    return progress;
}

TaskProgress ThreadPoolExecutor::build_task_progress_tree(
    TaskIndex task_id, std::unordered_set<TaskIndex>& processed) const {
    TaskProgress progress;

    if (processed.count(task_id)) {
        // Avoid cycles
        progress.task_id = task_id;
        progress.name = "[Cycle Detected]";
        return progress;
    }
    processed.insert(task_id);

    auto it = task_registry_.find(task_id);
    if (it == task_registry_.end()) {
        progress.task_id = task_id;
        progress.name = "[Not Found]";
        return progress;
    }

    const TaskInfo& info = it->second;
    progress.task_id = task_id;
    progress.name = info.name;

    // State
    switch (info.state) {
        case TaskInfo::QUEUED:
            progress.state = "queued";
            break;
        case TaskInfo::RUNNING:
            progress.state = "running";
            break;
        case TaskInfo::WAITING:
            progress.state = "waiting";
            break;
        case TaskInfo::COMPLETED:
            progress.state = "completed";
            break;
        case TaskInfo::FAILED:
            progress.state = "failed";
            break;
    }

    // Timing
    auto now = std::chrono::steady_clock::now();
    if (info.state == TaskInfo::QUEUED) {
        progress.queued_duration_ms =
            std::chrono::duration<double, std::milli>(now - info.queued_at)
                .count();
        progress.execution_duration_ms = 0;
    } else if (info.state == TaskInfo::RUNNING ||
               info.state == TaskInfo::WAITING) {
        progress.queued_duration_ms = std::chrono::duration<double, std::milli>(
                                          info.started_at - info.queued_at)
                                          .count();
        progress.execution_duration_ms =
            std::chrono::duration<double, std::milli>(now - info.started_at)
                .count();
    } else {  // COMPLETED or FAILED
        progress.queued_duration_ms = std::chrono::duration<double, std::milli>(
                                          info.started_at - info.queued_at)
                                          .count();
        progress.execution_duration_ms =
            std::chrono::duration<double, std::milli>(info.completed_at -
                                                      info.started_at)
                .count();
    }

    // Progress
    progress.total_subtasks = info.child_task_ids.size();
    progress.completed_subtasks = info.completed_children.load();
    if (progress.total_subtasks > 0) {
        progress.progress_percentage =
            (100.0 * static_cast<double>(progress.completed_subtasks)) /
            static_cast<double>(progress.total_subtasks);
    } else {
        progress.progress_percentage =
            (info.state == TaskInfo::COMPLETED) ? 100.0 : 0.0;
    }

    // Location
    switch (info.location) {
        case TaskInfo::SHARED_QUEUE:
            progress.location = "shared_queue";
            break;
        case TaskInfo::LOCAL_QUEUE:
            progress.location =
                "worker_" + std::to_string(info.worker_id) + "_local";
            break;
        case TaskInfo::EXECUTING:
            progress.location =
                "executing_on_worker_" + std::to_string(info.worker_id);
            break;
        case TaskInfo::DONE:
            progress.location = "done";
            break;
    }

    // Build children recursively
    for (TaskIndex child_id : info.child_task_ids) {
        progress.children.push_back(
            build_task_progress_tree(child_id, processed));
    }

    return progress;
}

// ============================================================================
// Phase 3: Coro-based task execution
// ============================================================================

coro::Coro ThreadPoolExecutor::run_task(std::shared_ptr<Task> task,
                                        std::shared_ptr<std::any> input) {
    // Get worker context from TLS (set by worker_thread at start).
    auto* context = static_cast<WorkerContext*>(get_current_worker_context());

    if (!context || !task) {
        co_return;
    }

    // Update worker context
    context->current_task_id = task->get_id();
    {
        std::lock_guard<std::mutex> lock(context->task_name_mutex);
        context->current_task_name = task->get_name();
    }
    context->last_activity = std::chrono::steady_clock::now();

    // Mark task start
    mark_activity();
    ++tasks_started_;

    // Update task registry to RUNNING
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = task_registry_.find(task->get_id());
        if (it != task_registry_.end()) {
            it->second.state = TaskInfo::RUNNING;
            it->second.started_at = std::chrono::steady_clock::now();
            it->second.worker_id = context->worker_id;
            it->second.location = TaskInfo::EXECUTING;
        }
    }

    task->result().mark_running();

    {
        CoroScope scope(this);
        std::exception_ptr task_error;

        DFTRACER_UTILS_LOG_DEBUG("Worker %zu executing task ID %ld ('%s')",
                                 context->worker_id, task->get_id(),
                                 task->get_name());

        try {
            auto coro_task = task->execute(scope, *input);
            // Propagate executor to the CoroTask's PromiseBase so that
            // nested awaitables (when_any, channel, etc.) can schedule
            // resumptions.  run_task() is a Coro (CoroPromise, not
            // PromiseBase), so the normal PromiseBase propagation in
            // CoroTask::await_suspend doesn't fire.
            coro_task.handle().promise().set_executor(this);
            std::any result = co_await std::move(coro_task);

            // Set result via TaskResult
            task->set_result(std::move(result));
        } catch (...) {
            task_error = std::current_exception();
        }

        // Always join scope to wait for any sub-spawned coroutines.
        // Without this, the scope's JoinHandle would be destroyed
        // while FinalAwaiters still reference it (use-after-free).
        co_await scope.join();

        // After the awaits we may have resumed on a different worker; the
        // starting `context` can be freed (retired), so touch the live resumer.
        auto* finishing =
            static_cast<WorkerContext*>(get_current_worker_context());

        if (!task_error) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Task ID %ld ('%s') completed successfully", task->get_id(),
                task->get_name());

            // Mark task completion
            mark_activity();
            ++tasks_completed_;
            if (finishing) finishing->tasks_executed++;

            // Update task registry to COMPLETED
            {
                std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                auto it = task_registry_.find(task->get_id());
                if (it != task_registry_.end()) {
                    it->second.state = TaskInfo::COMPLETED;
                    it->second.completed_at = std::chrono::steady_clock::now();
                    it->second.location = TaskInfo::DONE;

                    // Update parent's completed children count
                    if (it->second.parent_task_id != -1) {
                        auto parent_it =
                            task_registry_.find(it->second.parent_task_id);
                        if (parent_it != task_registry_.end()) {
                            parent_it->second.completed_children++;
                        }
                    }
                }
            }

            // Notify scheduler
            notify_completion(task);

        } else {
            try {
                std::rethrow_exception(task_error);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Task ID %ld ('%s') failed: %s",
                                         task->get_id(), task->get_name(),
                                         e.what());

                task->set_exception(task_error);

                mark_activity();
                ++tasks_completed_;
                if (finishing) finishing->tasks_executed++;

                {
                    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                    auto it = task_registry_.find(task->get_id());
                    if (it != task_registry_.end()) {
                        it->second.state = TaskInfo::FAILED;
                        it->second.completed_at =
                            std::chrono::steady_clock::now();
                        it->second.error_message = e.what();
                        it->second.location = TaskInfo::DONE;
                    }
                }

                notify_completion(task);

            } catch (...) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Task ID %ld ('%s') failed with unknown exception",
                    task->get_id(), task->get_name());

                task->set_exception(task_error);

                mark_activity();
                ++tasks_completed_;
                if (finishing) finishing->tasks_executed++;

                {
                    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                    auto it = task_registry_.find(task->get_id());
                    if (it != task_registry_.end()) {
                        it->second.state = TaskInfo::FAILED;
                        it->second.completed_at =
                            std::chrono::steady_clock::now();
                        it->second.error_message = "Unknown exception";
                        it->second.location = TaskInfo::DONE;
                    }
                }

                notify_completion(task);
            }
        }
    }

    // Clear current-task info only on the worker still showing this task: after
    // migration the starting worker's context may be freed or already onto its
    // next task.
    if (auto* cur = static_cast<WorkerContext*>(get_current_worker_context());
        cur && cur->current_task_id.load(std::memory_order_relaxed) ==
                   task->get_id()) {
        cur->current_task_id = -1;
        std::lock_guard<std::mutex> lock(cur->task_name_mutex);
        cur->current_task_name.clear();
    }

    co_return;
}

void ThreadPoolExecutor::submit_task(std::shared_ptr<Task> task,
                                     std::shared_ptr<std::any> input,
                                     TaskIndex parent_task_id) {
    // Register in task registry
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);

        auto [it, inserted] = task_registry_.emplace(
            std::piecewise_construct, std::forward_as_tuple(task->get_id()),
            std::forward_as_tuple());

        if (inserted) {
            it->second.task_id = task->get_id();
            it->second.parent_task_id = parent_task_id;
            it->second.name = task->get_name();
            it->second.state = TaskInfo::QUEUED;
            it->second.queued_at = std::chrono::steady_clock::now();
            it->second.location = TaskInfo::SHARED_QUEUE;
            it->second.worker_id = static_cast<std::size_t>(-1);

            if (parent_task_id != -1) {
                auto parent_it = task_registry_.find(parent_task_id);
                if (parent_it != task_registry_.end()) {
                    parent_it->second.child_task_ids.push_back(task->get_id());
                }
            }
        }
    }

    ++total_tasks_submitted_;

    // Create Coro, set executor on promise, enqueue released handle
    auto coro = run_task(std::move(task), std::move(input));
    coro.handle().promise().executor = this;
    enqueue(coro.release());
}

TaskIndex ThreadPoolExecutor::enqueue_tracked(
    coro::Coro coro, std::string name,
    std::shared_ptr<std::atomic<TaskIndex>> tid_out) {
    TaskIndex id = next_coro_task_id_.fetch_sub(1, std::memory_order_relaxed);
    coro.handle().promise().task_id = id;
    coro.handle().promise().executor = this;

    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto [it, inserted] = task_registry_.emplace(std::piecewise_construct,
                                                     std::forward_as_tuple(id),
                                                     std::forward_as_tuple());
        if (inserted) {
            auto now = std::chrono::steady_clock::now();
            it->second.task_id = id;
            it->second.parent_task_id = -1;
            it->second.name = std::move(name);
            it->second.state = TaskInfo::QUEUED;
            it->second.queued_at = now;
            it->second.location = TaskInfo::SHARED_QUEUE;
            it->second.worker_id = static_cast<std::size_t>(-1);
        }
    }
    ++total_tasks_submitted_;

    if (tid_out) {
        tid_out->store(id, std::memory_order_release);
    }

    auto handle = coro.release();
    enqueue(handle, id);
    return id;
}

TaskIndex ThreadPoolExecutor::current_worker_task_id() const {
    auto* ctx = static_cast<WorkerContext*>(get_current_worker_context());
    return ctx ? ctx->current_task_id.load() : -1;
}

void ThreadPoolExecutor::mark_coro_completed(TaskIndex id) {
    bool was_new = false;
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = task_registry_.find(id);
        if (it != task_registry_.end() &&
            it->second.state != TaskInfo::COMPLETED) {
            auto now = std::chrono::steady_clock::now();
            if (it->second.started_at.time_since_epoch().count() == 0) {
                it->second.started_at = now;
            }
            it->second.state = TaskInfo::COMPLETED;
            it->second.completed_at = now;
            it->second.location = TaskInfo::DONE;
            was_new = true;
        }
    }
    if (was_new) ++tasks_completed_;
}

void ThreadPoolExecutor::schedule_destroy(std::coroutine_handle<> handle) {
    if (handle) {
        destroy_queue_.enqueue(handle);
    }
}

void ThreadPoolExecutor::drain_destroy_queue() {
    std::coroutine_handle<> to_destroy;
    while (destroy_queue_.try_dequeue(to_destroy)) {
        if (to_destroy) {
            to_destroy.destroy();
        }
    }
}

}  // namespace dftracer::utils
