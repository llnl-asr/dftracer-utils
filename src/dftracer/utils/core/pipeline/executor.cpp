#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/io_executor.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <chrono>
#include <exception>

namespace dftracer::utils {

static thread_local void* tls_current_worker_context = nullptr;

void* get_current_worker_context() { return tls_current_worker_context; }

void set_current_worker_context(void* context) {
    tls_current_worker_context = context;
}

Executor::Executor(std::size_t num_threads, std::chrono::seconds idle_timeout,
                   std::chrono::seconds deadlock_timeout)
    : num_threads_(num_threads == 0 ? std::thread::hardware_concurrency()
                                    : num_threads),
      last_activity_time_(std::chrono::steady_clock::now()),
      idle_timeout_(idle_timeout),
      deadlock_timeout_(deadlock_timeout) {
    if (num_threads_ == 0) {
        num_threads_ = 2;  // Fallback if hardware_concurrency returns 0
    }
    DFTRACER_UTILS_LOG_DEBUG(
        "Executor created with %zu threads, idle_timeout=%lld s, "
        "deadlock_timeout=%lld s",
        num_threads_, idle_timeout_.count(), deadlock_timeout_.count());
}

Executor::~Executor() { shutdown(); }

void Executor::start() {
    if (running_) {
        DFTRACER_UTILS_LOG_WARN("%s", "Executor already running");
        return;
    }

    running_ = true;
    workers_.clear();
    workers_.reserve(num_threads_);

    timer_service_.start();

    // Create worker contexts and start threads
    for (std::size_t i = 0; i < num_threads_; ++i) {
        auto worker = std::make_unique<WorkerContext>(i);
        worker->last_activity = std::chrono::steady_clock::now();
        worker->thread =
            std::thread(&Executor::worker_thread, this, worker.get());
        workers_.push_back(std::move(worker));
    }

    DFTRACER_UTILS_LOG_DEBUG("Executor started with %zu worker threads",
                             num_threads_);
}

void Executor::shutdown() {
    // Shutdown I/O executor if it exists (do this even if executor not running)
    if (io_executor_) {
        io_executor_->shutdown();
        io_executor_.reset();
    }

    if (!running_) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutting down executor");
    running_ = false;

    // Wake up all workers
    for (auto& worker : workers_) {
        worker->cv.notify_all();
    }

    // Join all worker threads
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    workers_.clear();

    timer_service_.stop();

    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor shutdown complete");
}

void Executor::reset() {
    // Queue will be reset by caller if needed
    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor reset");
}

void Executor::set_completion_callback(CompletionCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    completion_callback_ = std::move(callback);
}

void Executor::worker_thread(WorkerContext* context) {
    DFTRACER_UTILS_LOG_DEBUG("Worker %zu started", context->worker_id);

    set_current_worker_context(context);

    while (running_) {
        TaskItem task;
        std::coroutine_handle<> pending_resume;

        // Priority order:
        // 0. Pending coroutine resumptions (highest priority for
        // responsiveness)
        if (pending_resumptions_.try_dequeue(pending_resume)) {
            context->is_idle = false;
            if (pending_resume && !pending_resume.done()) {
                pending_resume.resume();
            }
        }
        // 1. Own local queue (LIFO for cache locality)
        else if (try_pop_local(context, task)) {
            context->is_idle = false;
            drive_coroutine(execute_task(context, task), task.task);
        }
        // 3. Shared queue (for scheduler submissions)
        else if (shared_queue_.try_dequeue(task)) {
            context->is_idle = false;
            drive_coroutine(execute_task(context, task), task.task);
        }
        // 4. Steal from other workers (FIFO - oldest tasks)
        else if (try_steal_from_others(context, task)) {
            context->tasks_stolen++;
            context->is_idle = false;
            drive_coroutine(execute_task(context, task), task.task);
        }
        // 5. No work available
        else {
            context->is_idle = true;
            std::unique_lock<std::mutex> lock(context->queue_mutex);
            context->cv.wait_for(lock, std::chrono::milliseconds(10),
                                 [this] { return !running_.load(); });
        }
    }

    set_current_worker_context(nullptr);

    DFTRACER_UTILS_LOG_DEBUG("Worker %zu terminated", context->worker_id);
}

coro::CoroTask<void> Executor::execute_task(WorkerContext* context,
                                            TaskItem& item) {
    auto task = item.task;
    auto input = item.input;

    if (!task) {
        DFTRACER_UTILS_LOG_WARN("%s", "Null task in execute");
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

    // Update task registry
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

    try {
        TaskContext task_context(scheduler_, task->get_id(), this);

        // Execute task (co_await the coroutine)
        DFTRACER_UTILS_LOG_DEBUG("Worker %zu executing task ID %ld ('%s')",
                                 context->worker_id, task->get_id(),
                                 task->get_name().c_str());

        std::any result = co_await task->execute(task_context, *input);

        // Fulfill promise
        task->fulfill_promise(std::move(result));

        DFTRACER_UTILS_LOG_DEBUG("Task ID %ld ('%s') completed successfully",
                                 task->get_id(), task->get_name().c_str());

        // Mark task completion
        mark_activity();
        ++tasks_completed_;
        context->tasks_executed++;

        // Update task registry
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

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Task ID %ld ('%s') failed: %s",
                                 task->get_id(), task->get_name().c_str(),
                                 e.what());

        // Fulfill promise with exception
        task->fulfill_promise_exception(std::current_exception());

        // Mark task completion (even on error)
        mark_activity();
        ++tasks_completed_;
        context->tasks_executed++;

        // Update task registry
        {
            std::unique_lock<std::shared_mutex> lock(registry_mutex_);
            auto it = task_registry_.find(task->get_id());
            if (it != task_registry_.end()) {
                it->second.state = TaskInfo::FAILED;
                it->second.completed_at = std::chrono::steady_clock::now();
                it->second.error_message = e.what();
                it->second.location = TaskInfo::DONE;
            }
        }

        // Still notify scheduler (to handle error policy)
        notify_completion(task);

    } catch (...) {
        DFTRACER_UTILS_LOG_ERROR(
            "Task ID %ld ('%s') failed with unknown exception", task->get_id(),
            task->get_name().c_str());

        // Fulfill promise with exception
        task->fulfill_promise_exception(std::current_exception());

        // Mark task completion (even on error)
        mark_activity();
        ++tasks_completed_;
        context->tasks_executed++;

        // Update task registry
        {
            std::unique_lock<std::shared_mutex> lock(registry_mutex_);
            auto it = task_registry_.find(task->get_id());
            if (it != task_registry_.end()) {
                it->second.state = TaskInfo::FAILED;
                it->second.completed_at = std::chrono::steady_clock::now();
                it->second.error_message = "Unknown exception";
                it->second.location = TaskInfo::DONE;
            }
        }

        // Still notify scheduler
        notify_completion(task);
    }

    // Clear current task info
    context->current_task_id = -1;
    {
        std::lock_guard<std::mutex> lock(context->task_name_mutex);
        context->current_task_name.clear();
    }

    co_return;
}

void Executor::notify_completion(std::shared_ptr<Task> task) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (completion_callback_) {
        completion_callback_(task);
    }
}

void Executor::store_suspended_coro(TaskIndex awaited_task_id,
                                    std::unique_ptr<coro::CoroTask<void>> coro,
                                    std::shared_ptr<Task> suspended_task) {
    std::lock_guard<std::mutex> lock(suspended_coros_mutex_);

    DFTRACER_UTILS_LOG_DEBUG(
        "Storing suspended wrapper coroutine for task ID %d (waiting for task "
        "ID %d)",
        suspended_task->get_id(), awaited_task_id);

    suspended_coros_[awaited_task_id] =
        SuspendedCoro{.coro = std::move(coro),
                      .task_id = suspended_task->get_id(),
                      .task = suspended_task};
}

bool Executor::resume_suspended_coro(TaskIndex task_id) {
    std::unique_ptr<coro::CoroTask<void>> coro_to_resume;
    std::shared_ptr<Task> suspended_task;

    {
        std::lock_guard<std::mutex> lock(suspended_coros_mutex_);

        auto it = suspended_coros_.find(task_id);
        if (it == suspended_coros_.end()) {
            DFTRACER_UTILS_LOG_DEBUG(
                "No suspended coroutine found for task ID %d", task_id);
            return false;
        }

        // Extract the wrapper CoroTask and the suspended task
        coro_to_resume = std::move(it->second.coro);
        suspended_task = it->second.task;

        // Remove from map
        suspended_coros_.erase(it);
    }

    // Resume the wrapper CoroTask outside the lock
    // This will continue the execute_task coroutine, which will return control
    // to the user's co_await point, calling TaskFuture::await_resume()
    DFTRACER_UTILS_LOG_DEBUG(
        "Resuming suspended wrapper coroutine for task ID %d",
        suspended_task->get_id());

    if (coro_to_resume) {
        // root_promise is already set from initial drive_coroutine call

        // Clear the async flag since we're resuming
        coro_to_resume->set_awaiting_async(false);

        DFTRACER_UTILS_LOG_DEBUG(
            "About to resume coroutine for task ID %d in loop",
            suspended_task->get_id());

        // Resume the wrapper coroutine until completion or next suspension
        while (coro_to_resume && !coro_to_resume->done() &&
               !coro_to_resume->is_awaiting_async()) {
            coro_to_resume->resume();
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "Coroutine resume loop finished for task ID %d: done=%d, "
            "awaiting_async=%d",
            suspended_task->get_id(), coro_to_resume->done(),
            coro_to_resume->is_awaiting_async());

        // If it suspended again for another async operation, store it again
        if (coro_to_resume && !coro_to_resume->done() &&
            coro_to_resume->is_awaiting_async()) {
            // Get the NEW task ID we're waiting for from promise
            TaskIndex new_awaited_id =
                coro_to_resume->handle().promise().get_awaited_task_id();
            DFTRACER_UTILS_LOG_DEBUG(
                "Task ID %d suspended again waiting for task ID %d",
                suspended_task->get_id(), new_awaited_id);
            // Reset the awaited task id in promise before storing
            coro_to_resume->handle().promise().set_awaited_task_id(-1);
            store_suspended_coro(new_awaited_id, std::move(coro_to_resume),
                                 suspended_task);
        } else if (coro_to_resume && coro_to_resume->done()) {
            DFTRACER_UTILS_LOG_DEBUG("Task ID %d completed after resumption",
                                     suspended_task->get_id());
        }
    }

    return true;
}

void Executor::request_shutdown() {
    if (shutdown_requested_.load()) {
        return;  // Already requested
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutdown requested for executor");
    shutdown_requested_ = true;
}

void Executor::schedule_coroutine_resumption(std::coroutine_handle<> handle) {
    if (!handle || handle.done()) {
        return;  // Invalid or already completed
    }

    // Enqueue the coroutine handle for resumption by a worker thread
    pending_resumptions_.enqueue(handle);

    // Optionally wake up one idle worker to process the resumption
    // For now, workers will pick it up in their next iteration
}

// Forward declaration for when_all.h (avoids circular dependency)
void schedule_coroutine_resumption_helper(Executor* executor,
                                          std::coroutine_handle<> handle);

// Helper function for when_all.h (avoids circular dependency)
void schedule_coroutine_resumption_helper(Executor* executor,
                                          std::coroutine_handle<> handle) {
    if (executor) {
        executor->schedule_coroutine_resumption(handle);
    }
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

    // Check if we have pending tasks but no recent activity
    std::size_t queue_size = shared_queue_.size_approx();
    if (queue_size > 0) {
        // Get time since last activity
        std::lock_guard<std::mutex> lock(activity_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto idle_time = now - last_activity_time_;

        // If idle for more than idle_timeout with pending tasks, consider
        // unresponsive
        if (idle_time > idle_timeout_) {
            DFTRACER_UTILS_LOG_WARN(
                "Executor appears unresponsive: %zu tasks in queue, idle for "
                "%lld ms",
                queue_size,
                std::chrono::duration_cast<std::chrono::milliseconds>(idle_time)
                    .count());
            return false;
        }
    }

    // Check if all threads might be deadlocked
    // (all threads busy but no progress for a while)
    std::size_t started = tasks_started_.load();
    std::size_t completed = tasks_completed_.load();
    std::size_t active = started - completed;

    if (active >= num_threads_) {
        // All threads busy - check if making progress
        std::lock_guard<std::mutex> lock(activity_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto idle_time = now - last_activity_time_;

        // If all threads busy but no activity for deadlock_timeout, likely
        // deadlocked
        if (idle_time > deadlock_timeout_) {
            DFTRACER_UTILS_LOG_WARN(
                "Executor appears deadlocked: %zu threads, %zu active tasks, "
                "idle for %lld ms",
                num_threads_, active,
                std::chrono::duration_cast<std::chrono::milliseconds>(idle_time)
                    .count());
            return false;
        }
    }

    return true;
}

void Executor::mark_activity() {
    std::lock_guard<std::mutex> lock(activity_mutex_);
    last_activity_time_ = std::chrono::steady_clock::now();
}

bool Executor::try_steal_one_task() {
    // This is called by TaskFuture::get() for work-stealing
    // Get the current worker context
    auto* worker_context =
        static_cast<WorkerContext*>(get_current_worker_context());
    if (!worker_context) {
        // Not in a worker thread, can't steal
        return false;
    }

    TaskItem task;

    // Try to get work from:
    // 1. Own local queue first
    if (try_pop_local(worker_context, task)) {
        drive_coroutine(execute_task(worker_context, task), task.task);
        return true;
    }

    // 2. Shared queue
    if (shared_queue_.try_dequeue(task)) {
        drive_coroutine(execute_task(worker_context, task), task.task);
        return true;
    }

    // 3. Steal from others
    if (try_steal_from_others(worker_context, task)) {
        worker_context->tasks_stolen++;
        drive_coroutine(execute_task(worker_context, task), task.task);
        return true;
    }

    return false;
}

bool Executor::try_pop_local(WorkerContext* context, TaskItem& item) {
    std::lock_guard<std::mutex> lock(context->queue_mutex);
    if (!context->local_queue.empty()) {
        // Pop from back (LIFO for better cache locality)
        item = context->local_queue.back();
        context->local_queue.pop_back();
        return true;
    }
    return false;
}

bool Executor::try_steal_from_others(WorkerContext* thief, TaskItem& item) {
    // Guard against empty workers_ (can happen during shutdown or
    // initialization)
    if (workers_.empty() || workers_.size() == 1) {
        return false;
    }

    // Try to steal from other workers in round-robin fashion
    std::size_t start_idx = (thief->worker_id + 1) % workers_.size();

    for (std::size_t i = 0; i < workers_.size() - 1; ++i) {
        std::size_t victim_idx = (start_idx + i) % workers_.size();
        auto& victim = workers_[victim_idx];

        if (victim.get() == thief) continue;  // Don't steal from self

        std::unique_lock<std::mutex> lock(victim->queue_mutex,
                                          std::try_to_lock);
        if (!lock.owns_lock()) continue;

        if (!victim->local_queue.empty()) {
            // Steal from FRONT (oldest task - FIFO)
            item = victim->local_queue.front();
            victim->local_queue.pop_front();

            DFTRACER_UTILS_LOG_DEBUG("Worker %zu stole task from worker %zu",
                                     thief->worker_id, victim->worker_id);
            return true;
        }
    }

    return false;
}

void Executor::drive_coroutine(coro::CoroTask<void> coro,
                               std::shared_ptr<Task> task) {
    auto handle = coro.handle();
    if (!handle) {
        return;
    }

    TaskIndex initial_awaited_id = handle.promise().get_awaited_task_id();
    bool already_awaiting_async = (initial_awaited_id != -1) && !coro.done();

    auto& promise = handle.promise();
    promise.set_root_promise(&promise);
    promise.set_executor(this);

    if (!already_awaiting_async) {
        while (!coro.done() && !coro.is_awaiting_async()) {
            coro.resume();
        }
    }

    bool is_awaiting = already_awaiting_async || coro.is_awaiting_async();
    if (!coro.done() && is_awaiting) {
        // Read awaited_id from promise BEFORE moving coro
        TaskIndex awaited_id = handle.promise().get_awaited_task_id();

        DFTRACER_UTILS_LOG_DEBUG(
            "Coroutine for task ID %d suspended waiting for task ID %d, "
            "storing it",
            task->get_id(), awaited_id);

        // Reset the awaited task id in promise before storing
        handle.promise().set_awaited_task_id(-1);

        auto coro_ptr = std::make_unique<coro::CoroTask<void>>(std::move(coro));
        store_suspended_coro(awaited_id, std::move(coro_ptr), task);
    }
}

void Executor::submit_with_context(const TaskItem& item,
                                   TaskIndex parent_task_id,
                                   SubmissionHint hint) {
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);

        auto [it, inserted] =
            task_registry_.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(item.task->get_id()),
                                   std::forward_as_tuple());

        if (inserted) {
            it->second.task_id = item.task->get_id();
            it->second.parent_task_id = parent_task_id;
            it->second.name = item.task->get_name();
            it->second.state = TaskInfo::QUEUED;
            it->second.queued_at = std::chrono::steady_clock::now();
            it->second.location = TaskInfo::SHARED_QUEUE;
            it->second.worker_id = static_cast<std::size_t>(-1);

            if (parent_task_id != -1) {
                auto parent_it = task_registry_.find(parent_task_id);
                if (parent_it != task_registry_.end()) {
                    parent_it->second.child_task_ids.push_back(
                        item.task->get_id());
                }
            }
        }
    }

    ++total_tasks_submitted_;

    auto* worker_context =
        static_cast<WorkerContext*>(get_current_worker_context());

    if (hint == SubmissionHint::FORCE_SHARED || !worker_context) {
        // Submit to shared queue
        shared_queue_.enqueue(item);
        update_task_location(item.task->get_id(), TaskInfo::SHARED_QUEUE, -1);
    } else {
        // Submit to local queue of current worker
        {
            std::lock_guard<std::mutex> lock(worker_context->queue_mutex);
            worker_context->local_queue.push_back(item);
        }
        worker_context->cv.notify_one();
        update_task_location(item.task->get_id(), TaskInfo::LOCAL_QUEUE,
                             worker_context->worker_id);
    }
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
    progress.total_tasks_stolen = total_tasks_stolen_.load();

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

    // Queue depths
    progress.shared_queue_depth = shared_queue_.size_approx();
    for (const auto& worker : workers_) {
        std::lock_guard<std::mutex> queue_lock(worker->queue_mutex);
        progress.worker_queue_depths.push_back(worker->local_queue.size());
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

        {
            std::lock_guard<std::mutex> queue_lock(worker->queue_mutex);
            status.local_queue_depth = worker->local_queue.size();
        }

        progress.workers.push_back(status);
    }

    return progress;
}

std::optional<TaskProgress> Executor::get_task_progress(
    TaskIndex task_id) const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);

    auto it = task_registry_.find(task_id);
    if (it == task_registry_.end()) {
        return std::nullopt;
    }

    std::unordered_set<TaskIndex> processed;
    return build_task_progress_tree(task_id, processed);
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
// I/O Executor Management
// ============================================================================

void Executor::create_io_executor(std::size_t num_io_threads) {
    if (running_) {
        throw std::runtime_error(
            "Cannot create I/O executor while executor is running");
    }

    if (io_executor_) {
        throw std::runtime_error("I/O executor already exists");
    }

    if (num_io_threads == 0) {
        throw std::invalid_argument("num_io_threads must be > 0");
    }

    io_executor_ =
        IOExecutor::create(num_io_threads, num_threads_, &io_slow_path_queue_);

    // Start I/O threads
    io_executor_->start();
}

}  // namespace dftracer::utils
