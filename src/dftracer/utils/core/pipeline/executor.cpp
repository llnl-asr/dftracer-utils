#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <chrono>
#include <exception>

namespace dftracer::utils {

// Thread-local storage for current executor (for work-stealing)
static thread_local Executor* tls_current_executor = nullptr;
static thread_local void* tls_current_worker_context = nullptr;

// Helper functions to access thread-local executor
Executor* get_current_executor() { return tls_current_executor; }

void set_current_executor(Executor* exec) { tls_current_executor = exec; }

// Helper functions for worker context
void* get_current_worker_context() { return tls_current_worker_context; }

void set_current_worker_context(void* context) {
    tls_current_worker_context = context;
}

Executor::Executor(size_t num_threads)
    : num_threads_(num_threads == 0 ? std::thread::hardware_concurrency()
                                    : num_threads),
      last_activity_time_(std::chrono::steady_clock::now()) {
    if (num_threads_ == 0) {
        num_threads_ = 2;  // Fallback if hardware_concurrency returns 0
    }
    DFTRACER_UTILS_LOG_DEBUG("Executor created with %zu threads", num_threads_);
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

    // Create worker contexts and start threads
    for (size_t i = 0; i < num_threads_; ++i) {
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
    if (!running_) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutting down executor");
    running_ = false;
    shared_queue_.shutdown();

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

    // Set thread-local storage
    set_current_worker_context(context);
    set_current_executor(this);

    while (running_) {
        TaskItem task;

        // Priority order:
        // 1. Own local queue (LIFO for cache locality)
        if (try_pop_local(context, task)) {
            context->is_idle = false;
            execute_task(context, task);
        }
        // 2. Shared queue (for scheduler submissions)
        else if (auto item_opt = shared_queue_.pop(false)) {
            context->is_idle = false;
            execute_task(context, *item_opt);
        }
        // 3. Steal from other workers (FIFO - oldest tasks)
        else if (try_steal_from_others(context, task)) {
            context->tasks_stolen++;
            context->is_idle = false;
            execute_task(context, task);
        }
        // 4. No work available
        else {
            context->is_idle = true;
            std::unique_lock<std::mutex> lock(context->queue_mutex);
            context->cv.wait_for(lock, std::chrono::milliseconds(10),
                                 [this] { return !running_.load(); });
        }
    }

    // Clear thread-local storage
    set_current_worker_context(nullptr);
    set_current_executor(nullptr);

    DFTRACER_UTILS_LOG_DEBUG("Worker %zu terminated", context->worker_id);
}

void Executor::execute_task(WorkerContext* context, TaskItem& item) {
    auto task = item.task;
    auto input = item.input;

    if (!task) {
        DFTRACER_UTILS_LOG_WARN("%s", "Null task in execute");
        return;
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

    // ExecutorGuard ensures thread-local executor is set
    struct ExecutorGuard {
        Executor* prev;
        ExecutorGuard(Executor* exec) {
            prev = get_current_executor();
            set_current_executor(exec);
        }
        ~ExecutorGuard() { set_current_executor(prev); }
    };
    ExecutorGuard guard(this);

    try {
        // Create task context
        TaskContext task_context(scheduler_, task->get_id());

        // Execute task
        DFTRACER_UTILS_LOG_DEBUG("Worker %zu executing task ID %ld ('%s')",
                                 context->worker_id, task->get_id(),
                                 task->get_name().c_str());

        std::any result = task->execute(task_context, *input);

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
}

void Executor::notify_completion(std::shared_ptr<Task> task) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
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

    // Stop accepting new tasks
    shared_queue_.shutdown();
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
    size_t queue_size = shared_queue_.size();
    if (queue_size > 0) {
        // Get time since last activity
        std::lock_guard<std::mutex> lock(activity_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto idle_time = now - last_activity_time_;

        // If idle for more than 5 seconds with pending tasks, consider
        // unresponsive
        if (idle_time > std::chrono::seconds(5)) {
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
    size_t started = tasks_started_.load();
    size_t completed = tasks_completed_.load();
    size_t active = started - completed;

    if (active >= num_threads_) {
        // All threads busy - check if making progress
        std::lock_guard<std::mutex> lock(activity_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto idle_time = now - last_activity_time_;

        // If all threads busy but no activity for 10 seconds, likely deadlocked
        if (idle_time > std::chrono::seconds(10)) {
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
        execute_task(worker_context, task);
        return true;
    }

    // 2. Shared queue
    if (auto task_opt = shared_queue_.pop(false)) {
        execute_task(worker_context, *task_opt);
        return true;
    }

    // 3. Steal from others
    if (try_steal_from_others(worker_context, task)) {
        worker_context->tasks_stolen++;
        execute_task(worker_context, task);
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
    size_t start_idx = (thief->worker_id + 1) % workers_.size();

    for (size_t i = 0; i < workers_.size() - 1; ++i) {
        size_t victim_idx = (start_idx + i) % workers_.size();
        auto& victim = workers_[victim_idx];

        if (victim.get() == thief) continue;  // Don't steal from self

        // Try to lock victim's queue (non-blocking)
        std::unique_lock<std::mutex> lock(victim->queue_mutex,
                                          std::try_to_lock);
        if (!lock.owns_lock()) continue;  // Victim is busy, try next

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

void Executor::submit_with_context(const TaskItem& item,
                                   TaskIndex parent_task_id,
                                   SubmissionHint hint) {
    // Register task in the registry
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);

        // Construct TaskInfo in place
        auto [it, inserted] = task_registry_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(item.task->get_id()),
            std::forward_as_tuple()  // Default construct TaskInfo
        );

        if (inserted) {
            // Initialize the newly created TaskInfo
            it->second.task_id = item.task->get_id();
            it->second.parent_task_id = parent_task_id;
            it->second.name = item.task->get_name();
            it->second.state = TaskInfo::QUEUED;
            it->second.queued_at = std::chrono::steady_clock::now();
            it->second.location = TaskInfo::SHARED_QUEUE;
            it->second.worker_id = static_cast<size_t>(-1);

            // Update parent's child list
            if (parent_task_id != -1) {
                auto parent_it = task_registry_.find(parent_task_id);
                if (parent_it != task_registry_.end()) {
                    parent_it->second.child_task_ids.push_back(
                        item.task->get_id());
                }
            }
        }
    }

    // Increment total submitted count
    ++total_tasks_submitted_;

    // Decide where to submit
    auto* worker_context =
        static_cast<WorkerContext*>(get_current_worker_context());

    if (hint == SubmissionHint::FORCE_SHARED || !worker_context) {
        // Submit to shared queue
        shared_queue_.push(item);
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
                                    size_t worker_id) {
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

        // Collect recent errors
        if (info.state == TaskInfo::FAILED && !info.error_message.empty()) {
            progress.recent_errors.push_back({task_id, info.error_message});
        }
    }

    // Queue depths
    progress.shared_queue_depth = shared_queue_.size();
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

}  // namespace dftracer::utils
