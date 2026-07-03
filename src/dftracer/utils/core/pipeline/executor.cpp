#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/io/io_backend_factory.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/monitor.h>
#include <zlib.h>

#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
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

static thread_local void* tls_current_worker_context = nullptr;

void* get_current_worker_context() { return tls_current_worker_context; }

void set_current_worker_context(void* context) {
    tls_current_worker_context = context;
}

static thread_local Executor* tls_current_executor = nullptr;

Executor* Executor::current() noexcept { return tls_current_executor; }

Executor* Executor::set_current(Executor* e) noexcept {
    auto* old = tls_current_executor;
    tls_current_executor = e;
    return old;
}

// Thread-local list of coroutine handles to destroy after the current
// resume() returns.  FinalAwaiter pushes here instead of the shared
// destroy_queue_ to avoid another worker freeing the frame while
// the coroutine-suspend machinery is still accessing it.
static thread_local std::vector<std::coroutine_handle<>> tls_pending_destroys;

void schedule_thread_local_destroy(std::coroutine_handle<> h) {
    tls_pending_destroys.push_back(h);
}

void drain_thread_local_destroys() {
    Executor* exec = Executor::current();
    for (auto h : tls_pending_destroys) {
        if (!h) continue;
        if (exec) {
            exec->schedule_destroy(h);
        } else {
            h.destroy();
        }
    }
    tls_pending_destroys.clear();
}

Executor::Executor(const ExecutorConfig& config)
    : num_threads_(config.num_threads == 0
                       ? dftracer_utils_hardware_concurrency()
                       : config.num_threads),
      last_activity_ns_(
          std::chrono::steady_clock::now().time_since_epoch().count()),
      idle_timeout_(config.idle_timeout),
      deadlock_timeout_(config.deadlock_timeout),
      io_pool_size_(config.io_pool_size == 0
                        ? dftracer_utils_hardware_concurrency()
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
    DFTRACER_UTILS_LOG_DEBUG(
        "Executor created with %zu threads, idle_timeout=%lld s, "
        "deadlock_timeout=%lld s",
        num_threads_, static_cast<long long>(idle_timeout_.count()),
        static_cast<long long>(deadlock_timeout_.count()));
}

Executor::~Executor() {
    shutdown();
    drain_destroy_queue();
}

void Executor::start() {
    if (running_) {
        DFTRACER_UTILS_LOG_WARN("%s", "Executor already running");
        return;
    }

    running_ = true;
    workers_.clear();
    workers_.reserve(num_threads_);

    static std::once_flag warmup_once;
    std::call_once(warmup_once, warmup_vendored_libs);

    timer_service_.start();

    // Create and start I/O backend before workers so workers
    // can use it immediately.
    io_backend_ = io::create_io_backend(*this, io_pool_size_, io_backend_type_,
                                        io_batch_threshold_);
    io_backend_->start();

    // Create all worker contexts first so workers_ is stable before any
    // worker thread can try to iterate/steal from it.
    for (std::size_t i = 0; i < num_threads_; ++i) {
        auto worker = std::make_unique<WorkerContext>(i);
        worker->last_activity = std::chrono::steady_clock::now();
        workers_.push_back(std::move(worker));
    }

    // Start worker threads after all contexts are in place.
    for (auto& worker : workers_) {
        worker->thread =
            std::thread(&Executor::worker_thread, this, worker.get());
    }

    DFTRACER_UTILS_LOG_DEBUG("Executor started with %zu worker threads",
                             num_threads_);
}

void Executor::shutdown() {
    if (!running_) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutting down executor");
    running_ = false;
    wake_all_workers();

    // Join all worker threads (must happen before io_backend_ is
    // destroyed, since workers call io_backend_->poll() when idle).
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    // Stop I/O backend AFTER joining workers (workers may poll the
    // backend) but BEFORE clearing workers_.  The I/O backend's
    // completion thread may still call enqueue() -> wake_all_workers()
    // which accesses WorkerContext cv/mutex, so workers_ must remain
    // alive until the completion thread has exited.
    if (io_backend_) {
        io_backend_->stop();
        io_backend_.reset();
    }

    // Destroy deferred frames and orphaned run-queue entries BEFORE
    // clearing workers_. Frames may hold shared_ptr<Channel> whose
    // ConcurrentQueue has TLS producer tokens tied to worker threads.
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
    timer_service_.stop();

    // Drain the main thread's thread-local destroy list (for
    // coroutines whose FinalAwaiter ran on the main thread).
    drain_thread_local_destroys();

    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor shutdown complete");
}

void Executor::reset() {
    // Queue will be reset by caller if needed
    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor reset");
}

void Executor::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

void Executor::worker_thread(WorkerContext* context) {
    DFTRACER_UTILS_LOG_DEBUG("Worker %zu started", context->worker_id);

    set_current_worker_context(context);
    tls_current_executor = this;
    coro::reset_timeslice();

    // Fixed for the process lifetime; read once to keep the resume loop cheap.
    const bool monitor = utilities::monitoring_enabled();
    if (monitor) {
        utilities::monitor_set_worker(static_cast<int>(context->worker_id));
    }

    while (running_) {
        RunQueueEntry pending_entry;

        // Snapshot the work signal BEFORE checking any queues.
        // This ensures that any signal increment (from enqueue +
        // signal_global_work) that happens AFTER this load will be detected by
        // the wait predicate below, even if the actual queue check sees the
        // queue as empty. Loading it inside the else branch (after queue
        // checks) creates a race: work can arrive between the queue check and
        // the signal load, causing the worker to sleep with the updated signal
        // value while work sits in the queue.
        const std::uint64_t observed_signal =
            work_signal_.load(std::memory_order_acquire);

        // Run queue: coroutine handles from enqueue() and
        // schedule_coroutine_resumption().
        if (run_queue_.try_dequeue(pending_entry)) {
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
            // This ensures pending SQEs are submitted even when no
            // new work is arriving (drain trigger).
            if (io_backend_) {
                io_backend_->flush();
            }
            // Opportunistic I/O polling before sleeping.
            // If the backend has completions, they'll enqueue new
            // work, so skip the sleep and retry the run queue.
            if (io_backend_) {
                auto reaped = io_backend_->poll(0);
                if (reaped > 0) {
                    drain_destroy_queue();
                    continue;
                }
            }
            drain_destroy_queue();
            work_signal_.wait(observed_signal, std::memory_order_acquire);
        }
    }

    // Final drain: destroy any frames deferred during the last resume().
    drain_thread_local_destroys();

    tls_current_executor = nullptr;
    set_current_worker_context(nullptr);

    DFTRACER_UTILS_LOG_DEBUG("Worker %zu terminated", context->worker_id);
}

void Executor::notify_completion(std::shared_ptr<Task> task) {
    // No mutex needed -- the callback is set exactly once during Scheduler
    // construction (before any task is submitted) and never modified after.
    // on_task_completed uses only atomics, lock-free queues, and properly-
    // locked shard mutexes, so concurrent calls from multiple workers are safe.
    if (completion_callback_) {
        completion_callback_(task);
    }
}

void Executor::request_shutdown() {
    if (shutdown_requested_.load()) {
        return;  // Already requested
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutdown requested for executor");
    shutdown_requested_ = true;
}

void Executor::schedule_coroutine_resumption(std::coroutine_handle<> handle) {
    // Delegate to enqueue() -- unified path for all coroutine handles.
    enqueue(handle);
}

void Executor::signal_global_work() {
    work_signal_.fetch_add(1, std::memory_order_acq_rel);
    // Wake one worker, not all. Each enqueue adds one unit of work,
    // so one worker is sufficient. Avoids thundering herd where all
    // N threads wake, N-1 find no work, and go back to sleep.
    wake_one_worker();
}

void Executor::wake_one_worker() { work_signal_.notify_one(); }

void Executor::wake_all_workers() {
    work_signal_.fetch_add(1, std::memory_order_release);
    work_signal_.notify_all();
}

// Helper function for when_all.h (avoids circular dependency)
void schedule_coroutine_resumption_helper(Executor* executor,
                                          std::coroutine_handle<> handle) {
    if (executor) {
        executor->schedule_coroutine_resumption(handle);
    }
}

// Helper function for when_any.h (avoids circular dependency)
void schedule_destroy_helper(Executor* executor,
                             std::coroutine_handle<> handle) {
    if (executor && handle) {
        executor->schedule_destroy(handle);
    }
}

void Executor::enqueue(std::coroutine_handle<> handle, TaskIndex task_id) {
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

bool Executor::is_responsive() const {
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

void Executor::mark_activity() {
    last_activity_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_release);
}

void Executor::update_task_location(TaskIndex task_id,
                                    TaskInfo::Location location,
                                    std::size_t worker_id) {
    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
    auto it = task_registry_.find(task_id);
    if (it != task_registry_.end()) {
        it->second.location = location;
        if (location == TaskInfo::LOCAL_QUEUE ||
            location == TaskInfo::EXECUTING) {
            it->second.worker_id = worker_id;
        }
    }
}

ExecutorProgress Executor::get_progress() const {
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

    // Worker states
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

    return progress;
}

TaskProgress Executor::build_task_progress_tree(
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

coro::Coro Executor::run_task(std::shared_ptr<Task> task,
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

        if (!task_error) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Task ID %ld ('%s') completed successfully", task->get_id(),
                task->get_name());

            // Mark task completion
            mark_activity();
            ++tasks_completed_;
            context->tasks_executed++;

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
                context->tasks_executed++;

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
                context->tasks_executed++;

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

    // Clear current task info
    context->current_task_id = -1;
    {
        std::lock_guard<std::mutex> lock(context->task_name_mutex);
        context->current_task_name.clear();
    }

    co_return;
}

void Executor::submit_task(std::shared_ptr<Task> task,
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

TaskIndex Executor::enqueue_tracked(
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

void Executor::mark_coro_completed(TaskIndex id) {
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

void Executor::schedule_destroy(std::coroutine_handle<> handle) {
    if (handle) {
        destroy_queue_.enqueue(handle);
    }
}

void Executor::drain_destroy_queue() {
    std::coroutine_handle<> to_destroy;
    while (destroy_queue_.try_dequeue(to_destroy)) {
        if (to_destroy) {
            to_destroy.destroy();
        }
    }
}

}  // namespace dftracer::utils
