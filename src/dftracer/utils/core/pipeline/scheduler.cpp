#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/tasks/task.h>

#include <algorithm>
#include <any>
#include <exception>
#include <tuple>
#include <vector>

namespace dftracer::utils {

Scheduler::Scheduler(Executor* executor)
    : executor_(executor),
      num_scheduling_threads_(1),
      metrics_start_time_(std::chrono::steady_clock::now()) {
    if (!executor_) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Executor cannot be null");
    }

    // Set this scheduler as the executor's completion callback
    executor_->set_completion_callback([this](std::shared_ptr<Task> task) {
        try {
            on_task_completed(task);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Exception in on_task_completed for task '%s': %s",
                task->get_name().c_str(), e.what());
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown exception in on_task_completed for task '%s'",
                task->get_name().c_str());
        }
    });

    // Set scheduler reference in executor (for TaskContext)
    executor_->set_scheduler(this);

    // Start executor to ensure workers are ready before scheduling begins
    if (!executor_->is_running()) {
        executor_->start();
    }

    // Start scheduling thread
    start_scheduling_thread();
}

Scheduler::Scheduler(Executor* executor, const PipelineConfigManager& config)
    : executor_(executor),
      num_scheduling_threads_(
          config.scheduler_threads > 0 ? config.scheduler_threads : 1),
      global_timeout_(config.global_timeout),
      default_task_timeout_(config.default_task_timeout),
      error_policy_(config.error_policy),
      error_handler_(config.error_handler),
      metrics_start_time_(std::chrono::steady_clock::now()) {
    if (!executor_) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Executor cannot be null");
    }

    // Set completion callback
    executor_->set_completion_callback([this](std::shared_ptr<Task> task) {
        try {
            on_task_completed(task);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Exception in on_task_completed for task '%s': %s",
                task->get_name().c_str(), e.what());
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown exception in on_task_completed for task '%s'",
                task->get_name().c_str());
        }
    });

    // Set scheduler reference
    executor_->set_scheduler(this);

    // Start executor to ensure workers are ready before scheduling begins
    if (!executor_->is_running()) {
        executor_->start();
    }

    // Create watchdog if enabled
    if (config.enable_watchdog) {
        watchdog_ = std::make_unique<Watchdog>(
            config.watchdog_interval, config.global_timeout,
            config.default_task_timeout, config.long_task_warning_threshold);

        // Set watchdog callbacks
        watchdog_->set_executor(executor_);

        watchdog_->set_timeout_callback([this](const std::string& reason) {
            DFTRACER_UTILS_LOG_ERROR("Watchdog timeout: %s", reason.c_str());
            // Set error flag first, then request shutdown
            // This ensures we throw TIMEOUT_ERROR instead of INTERRUPTED
            has_error_ = true;
            timeout_reason_ = reason;
            request_shutdown();
        });

        watchdog_->set_warning_callback(
            [](const std::string& task_name, int64_t elapsed_ms) {
                DFTRACER_UTILS_LOG_WARN("Long-running task: %s (%lld ms)",
                                        task_name.c_str(), elapsed_ms);
            });
    }

    // Start scheduling threads
    start_scheduling_thread();

    DFTRACER_UTILS_LOG_DEBUG(
        "Scheduler created with %zu scheduling threads, watchdog: %s",
        num_scheduling_threads_, watchdog_ ? "enabled" : "disabled");
}

Scheduler::~Scheduler() {
    // Shutdown executor first to ensure all worker threads stop before
    // we destroy the scheduler (workers may call scheduler methods)
    if (executor_) {
        executor_->shutdown();
    }

    stop_scheduling_thread();

    if (watchdog_) {
        watchdog_->stop();
    }
}

void Scheduler::schedule(std::shared_ptr<Task> source, const std::any& input) {
    if (!source) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Source task cannot be null");
    }

    // Validate task graph types
    validate_task_types(source);

    // Reset state
    {
        std::lock_guard<std::mutex> lock(tracking_mutex_);
        pending_count_ = 0;
        has_error_ = false;
        shutdown_requested_ = false;
        timeout_reason_.clear();
    }

    running_ = true;
    execution_start_time_ = std::chrono::steady_clock::now();

    // Start watchdog if available
    if (watchdog_) {
        watchdog_->mark_execution_start();
        watchdog_->start();
    }

    // Count total tasks
    total_tasks_ = count_reachable_tasks(source);
    DFTRACER_UTILS_LOG_DEBUG("Scheduling %zu tasks", total_tasks_.load());

    // Initialize pending counts
    initialize_pending_counts(source);

    // Start executor if not running
    if (!executor_->is_running()) {
        executor_->start();
    }

    // Set initial input on source task if not already set
    if (!source->has_initial_input()) {
        source->set_initial_input(input);
    }

    // Increment pending count for source task
    ++pending_count_;

    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        ready_queue_.push(source);
    }
    ready_cv_.notify_one();

    // Wait for completion WITH TIMEOUT
    {
        std::unique_lock<std::mutex> lock(done_mutex_);

        if (global_timeout_.count() > 0) {
            // Timed wait
            bool completed = done_cv_.wait_for(lock, global_timeout_, [this] {
                return pending_count_ == 0 ||
                       (has_error_.load() &&
                        error_policy_ == ErrorPolicy::FAIL_FAST) ||
                       shutdown_requested_.load();
            });

            if (!completed && !shutdown_requested_.load()) {
                // Timeout occurred
                DFTRACER_UTILS_LOG_ERROR("Pipeline timed out after %lld ms",
                                         global_timeout_.count());
                request_shutdown();
                throw PipelineError(PipelineError::TIMEOUT_ERROR,
                                    "Pipeline execution timed out");
            }
        } else {
            // Infinite wait (current behavior)
            done_cv_.wait(lock, [this] {
                return pending_count_ == 0 ||
                       (has_error_.load() &&
                        error_policy_ == ErrorPolicy::FAIL_FAST) ||
                       shutdown_requested_.load();
            });
        }
    }

    running_ = false;

    // Stop watchdog
    if (watchdog_) {
        watchdog_->stop();
    }

    // Check for errors
    // Check for timeout first (watchdog sets both has_error_ and
    // shutdown_requested_)
    if (!timeout_reason_.empty()) {
        throw PipelineError(PipelineError::TIMEOUT_ERROR, timeout_reason_);
    }

    if (shutdown_requested_.load()) {
        throw PipelineError(PipelineError::INTERRUPTED,
                            "Pipeline execution was interrupted");
    }

    if (has_error_.load() && error_policy_ == ErrorPolicy::FAIL_FAST) {
        DFTRACER_UTILS_LOG_DEBUG(
            "Throwing EXECUTION_ERROR: has_error=%d, policy=%d",
            has_error_.load(), static_cast<int>(error_policy_));
        throw PipelineError(PipelineError::EXECUTION_ERROR,
                            "Pipeline execution failed");
    }

    DFTRACER_UTILS_LOG_DEBUG("Scheduling complete", "");
}

void Scheduler::on_task_completed(std::shared_ptr<Task> task) {
    if (!task) {
        return;
    }

    // If shutdown was requested, do minimal cleanup and return immediately
    // to avoid accessing potentially destroyed state
    if (shutdown_requested_.load()) {
        --pending_count_;
        if (pending_count_ == 0) {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_cv_.notify_all();
        }
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("Task ID %d ('%s') completed notification",
                             task->get_id(), task->get_name().c_str());

    // Unregister from watchdog
    if (watchdog_) {
        watchdog_->unregister_task(task->get_id());
    }

    // Check if task failed
    bool task_failed = false;
    try {
        // Try to get the future
        // if it has an exception, this will throw
        auto future = task->get_future();
        if (future.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
            try {
                future.get();
            } catch (...) {
                task_failed = true;
                has_error_ = true;
                handle_task_error(task);
                // Note: Don't call request_shutdown() here
                // let schedule() throw EXECUTION_ERROR instead of INTERRUPTED
            }
        }
    } catch (...) {
        task_failed = true;
        has_error_ = true;
        handle_task_error(task);
    }

    // Don't schedule children if shutdown was requested OR if task failed with
    // FAIL_FAST policy
    if (shutdown_requested_.load() ||
        (task_failed && error_policy_ == ErrorPolicy::FAIL_FAST)) {
        DFTRACER_UTILS_LOG_DEBUG(
            "Skipping child scheduling for task '%s' due to shutdown or "
            "FAIL_FAST",
            task->get_name().c_str());
        // Decrement pending count for this completed task
        --pending_count_;
        if (pending_count_ == 0) {
            done_cv_.notify_all();
        }
        return;
    }

    // Make a copy of children to avoid iterator invalidation or use-after-free
    // if task is destroyed during iteration
    auto children = task->get_children();

    // Schedule children (add to ready queue)
    // Note: Even if this task failed with CONTINUE/CUSTOM policy, we still
    // process children normally. When a child becomes ready, we'll check if any
    // parent failed and skip it at that point.
    for (const auto& child : children) {
        if (!child) {
            continue;  // Skip null children
        }

        child->decrement_pending_parents();

        if (child->is_ready()) {
            // For CONTINUE/CUSTOM policy: skip children if any parent failed
            bool has_failed_parent = false;
            if (error_policy_ != ErrorPolicy::FAIL_FAST && executor_) {
                for (const auto& parent : child->get_parents()) {
                    auto parent_progress =
                        executor_->get_task_progress(parent->get_id());
                    if (parent_progress.has_value() &&
                        parent_progress->state == "failed") {
                        has_failed_parent = true;
                        break;
                    }
                }
            }

            if (has_failed_parent) {
                DFTRACER_UTILS_LOG_WARN(
                    "Skipping child task ID %d ('%s') because parent failed "
                    "(CONTINUE policy)",
                    child->get_id(), child->get_name().c_str());

                // Mark child as failed and propagate to its children
                skip_task_and_descendants(child);
                continue;
            }

            // Increment pending count for this child
            ++pending_count_;

            // Add to ready queue
            {
                std::lock_guard<std::mutex> lock(ready_mutex_);
                ready_queue_.push(child);
            }
            ready_cv_.notify_one();
        }
    }

    // Decrement pending count
    // Only if not already decremented by handle_task_error
    if (!task_failed) {
        --pending_count_;
        if (pending_count_ == 0) {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_cv_.notify_all();
        }
    }

    // Report progress using executor's metrics
    if (progress_callback_) {
        auto executor_progress = executor_->get_progress();
        progress_callback_(executor_progress.tasks_completed, total_tasks_);
    }
}

void Scheduler::submit_dynamic_task(std::shared_ptr<Task> task,
                                    const std::any& input) {
    if (!running_) {
        DFTRACER_UTILS_LOG_WARN("%s",
                                "Cannot submit dynamic task when not running");
        return;
    }

    // Initialize pending count for dynamic task
    task->initialize_pending_count();

    // Increment total tasks
    ++total_tasks_;

    // Increment pending count BEFORE submitting to executor
    // This prevents the scheduler from thinking all work is done
    // before the dynamic task even starts
    ++pending_count_;

    // Submit to executor
    submit_task_to_executor(task, input);
}

void Scheduler::set_error_policy(ErrorPolicy policy) { error_policy_ = policy; }

void Scheduler::set_error_handler(ErrorHandler handler) {
    error_handler_ = std::move(handler);
}

void Scheduler::set_progress_callback(
    std::function<void(size_t completed, size_t total)> callback) {
    progress_callback_ = std::move(callback);
}

void Scheduler::reset() {
    std::lock_guard<std::mutex> lock(tracking_mutex_);
    pending_count_ = 0;
    total_tasks_ = 0;
    has_error_ = false;
}

bool Scheduler::is_task_completed(TaskIndex task_id) const {
    // Query executor's task registry for authoritative task state
    auto progress = executor_->get_task_progress(task_id);
    if (progress.has_value()) {
        return progress->state == "completed";
    }
    return false;
}

void Scheduler::schedule_ready_children(std::shared_ptr<Task> completed_task) {
    for (auto& child : completed_task->get_children()) {
        // Decrement pending parents count
        child->decrement_pending_parents();

        // Check if child is now ready
        if (child->is_ready() && !is_task_completed(child->get_id())) {
            // For CONTINUE/CUSTOM policy: skip children if any parent failed
            bool has_failed_parent = false;
            if (error_policy_ != ErrorPolicy::FAIL_FAST && executor_) {
                for (const auto& parent : child->get_parents()) {
                    auto parent_progress =
                        executor_->get_task_progress(parent->get_id());
                    if (parent_progress.has_value() &&
                        parent_progress->state == "failed") {
                        has_failed_parent = true;
                        break;
                    }
                }
            }

            if (has_failed_parent) {
                DFTRACER_UTILS_LOG_WARN(
                    "Skipping child task ID %d ('%s') because parent failed "
                    "(CONTINUE policy)",
                    child->get_id(), child->get_name().c_str());

                // Mark child as failed too
                child->fulfill_promise_exception(
                    std::make_exception_ptr(PipelineError(
                        PipelineError::EXECUTION_ERROR, "Parent task failed")));
                --pending_count_;
                continue;
            }

            DFTRACER_UTILS_LOG_DEBUG(
                "Child task ID %d ('%s') is ready for execution",
                child->get_id(), child->get_name().c_str());

            try {
                // Prepare input: use initial_input if set, otherwise from
                // parents
                std::any input;
                if (child->has_initial_input()) {
                    input = child->get_initial_input().value();
                    DFTRACER_UTILS_LOG_DEBUG(
                        "Using initial input for task ID %d", child->get_id());
                } else {
                    input = prepare_input_for_task(child);
                }

                // Submit to executor
                submit_task_to_executor(child, input);
            } catch (...) {
                // Error during input preparation
                // (e.g., combiner validation error)
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to prepare input for task ID %d ('%s')",
                    child->get_id(), child->get_name().c_str());

                // Store the exception in the task's promise
                child->fulfill_promise_exception(std::current_exception());

                // Handle the error (calls custom handler, checks error policy)
                handle_task_error(child);
            }
        }
    }
}

bool Scheduler::all_parents_completed(std::shared_ptr<Task> task) const {
    for (const auto& parent : task->get_parents()) {
        if (!is_task_completed(parent->get_id())) {
            return false;
        }
    }
    return true;
}

std::any Scheduler::prepare_input_for_task(std::shared_ptr<Task> task) {
    const auto& parents = task->get_parents();

    if (parents.empty()) {
        // No parents - check for initial input
        if (task->has_initial_input()) {
            return task->get_initial_input().value();
        }
        return std::any{};
    }

    if (parents.size() == 1 && !task->has_combiner()) {
        // Single parent without combiner
        auto future = parents[0]->get_future();
        return future.get();
    }

    // Single parent with combiner OR multiple parents
    // check if task has custom combiner
    if (task->has_combiner()) {
        std::vector<std::any> parent_outputs;
        parent_outputs.reserve(parents.size());
        for (const auto& parent : parents) {
            auto future = parent->get_future();
            parent_outputs.push_back(future.get());
        }
        try {
            return task->apply_combiner(parent_outputs);
        } catch (...) {
            // Combiner validation error
            // rethrow to be handled by caller
            throw;
        }
    }

    // Default: tuple packing
    // Check if all parents have void output (synchronization only)
    bool all_void = std::all_of(
        parents.begin(), parents.end(),
        [](const auto& p) { return p->get_output_type() == typeid(void); });

    if (all_void) {
        // All void - return void
        return std::any{};
    }

    // Pack parent outputs into a tuple for type-safe multi-arg functions
    // The Task's wrap_function will handle unpacking the tuple
    std::vector<std::any> parent_outputs;
    parent_outputs.reserve(parents.size());
    for (const auto& parent : parents) {
        auto future = parent->get_future();
        parent_outputs.push_back(future.get());
    }

    // Create tuple based on number of parents
    // The actual tuple type will be checked by Task's type system
    switch (parents.size()) {
        case 2: {
            return std::any(
                std::make_tuple(parent_outputs[0], parent_outputs[1]));
        }
        case 3: {
            return std::any(std::make_tuple(
                parent_outputs[0], parent_outputs[1], parent_outputs[2]));
        }
        case 4: {
            return std::any(
                std::make_tuple(parent_outputs[0], parent_outputs[1],
                                parent_outputs[2], parent_outputs[3]));
        }
        case 5: {
            return std::any(std::make_tuple(
                parent_outputs[0], parent_outputs[1], parent_outputs[2],
                parent_outputs[3], parent_outputs[4]));
        }
        default:
            // Fallback to vector for >5 parents
            return std::any(parent_outputs);
    }
}

void Scheduler::submit_task_to_executor(std::shared_ptr<Task> task,
                                        std::any input) {
    DFTRACER_UTILS_LOG_DEBUG("Submitting task ID %ld ('%s') to executor",
                             task->get_id(), task->get_name().c_str());

    // @Note: pending_count_ is incremented when task is added to ready queue,
    // not here, to avoid race condition with wait predicate

    // Create task item
    auto input_ptr = std::make_shared<std::any>(std::move(input));
    TaskItem item{task, input_ptr};

    // Determine parent task ID for tracking
    // For dynamic tasks, we get the parent from the current context
    // For regular pipeline tasks, the parents are tracked differently
    TaskIndex parent_id = -1;

    // Check if this is a dynamic submission (called from TaskContext)
    // The parent task ID would be available from the current task context
    // We can get it from the thread-local executor context if we're in a task
    auto* worker_context =
        static_cast<Executor::WorkerContext*>(get_current_worker_context());
    if (worker_context) {
        parent_id = worker_context->current_task_id.load();
    }

    // Use the new submit_with_context for better tracking
    executor_->submit_with_context(item, parent_id);

    // Track total scheduled
    ++total_scheduled_;
}

size_t Scheduler::count_reachable_tasks(std::shared_ptr<Task> source) {
    std::unordered_set<TaskIndex> visited;
    size_t count = 0;
    count_tasks_dfs(source, visited, count);
    return count;
}

void Scheduler::count_tasks_dfs(std::shared_ptr<Task> task,
                                std::unordered_set<TaskIndex>& visited,
                                size_t& count) {
    if (!task || visited.find(task->get_id()) != visited.end()) {
        return;
    }

    visited.insert(task->get_id());
    ++count;

    for (const auto& child : task->get_children()) {
        count_tasks_dfs(child, visited, count);
    }
}

void Scheduler::initialize_pending_counts(std::shared_ptr<Task> source) {
    std::unordered_set<TaskIndex> visited;
    initialize_pending_counts_dfs(source, visited);
}

void Scheduler::initialize_pending_counts_dfs(
    std::shared_ptr<Task> task, std::unordered_set<TaskIndex>& visited) {
    if (!task || visited.find(task->get_id()) != visited.end()) {
        return;
    }

    visited.insert(task->get_id());

    // Initialize pending count
    task->initialize_pending_count();

    for (const auto& child : task->get_children()) {
        initialize_pending_counts_dfs(child, visited);
    }
}

void Scheduler::handle_task_error(std::shared_ptr<Task> task) {
    DFTRACER_UTILS_LOG_ERROR("Task ID %d ('%s') failed", task->get_id(),
                             task->get_name().c_str());

    has_error_ = true;

    // If shutdown was requested, skip error handler and just dec pending count
    if (shutdown_requested_.load()) {
        --pending_count_;
        std::lock_guard<std::mutex> lock(done_mutex_);
        done_cv_.notify_all();
        return;
    }

    // Call custom error handler if provided
    if (error_handler_) {
        try {
            auto future = task->get_future();
            std::exception_ptr ex = nullptr;
            try {
                future.get();
            } catch (...) {
                ex = std::current_exception();
            }
            error_handler_(task, ex);
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR("%s", "Error handler threw exception");
        }
    }

    if (error_policy_ == ErrorPolicy::FAIL_FAST) {
        // Stop scheduling new tasks
        --pending_count_;  // Still decrement to unblock waiting thread

        std::lock_guard<std::mutex> lock(done_mutex_);
        done_cv_.notify_all();
    } else {
        // CONTINUE or CUSTOM: decrement pending count and let other branches
        // continue
        --pending_count_;

        // Notify if all tasks are done
        if (pending_count_ == 0) {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_cv_.notify_all();
        }
    }
}

void Scheduler::validate_task_types(std::shared_ptr<Task> task) {
    // BFS to validate types for this task and all descendants
    std::unordered_set<TaskIndex> visited;
    std::queue<std::shared_ptr<Task>> queue;

    queue.push(task);
    visited.insert(task->get_id());

    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();

        // Validate this task's parent connections
        for (const auto& parent : current->get_parents()) {
            // Skip validation for void outputs (synchronization only)
            if (parent->get_output_type() == typeid(void)) {
                continue;
            }

            // For single parent without combiner, types should match
            if (current->get_parents().size() == 1 &&
                !current->has_combiner()) {
                if (parent->get_output_type() != current->get_input_type()) {
                    std::string parent_type =
                        Task::demangle_type_name(parent->get_output_type());
                    std::string task_type =
                        Task::demangle_type_name(current->get_input_type());
                    throw PipelineError(
                        PipelineError::TYPE_MISMATCH,
                        "Type mismatch: task '" + current->get_name() +
                            "' expects " + task_type + " but parent '" +
                            parent->get_name() + "' outputs " + parent_type);
                }
            }
        }

        // Add children to queue for BFS
        for (const auto& child : current->get_children()) {
            if (child && !visited.count(child->get_id())) {
                visited.insert(child->get_id());
                queue.push(child);
            }
        }
    }
}

void Scheduler::skip_task_and_descendants(std::shared_ptr<Task> task) {
    if (!task) {
        return;
    }

    // Check if already processed using executor's registry
    auto progress = executor_->get_task_progress(task->get_id());
    if (progress.has_value() &&
        (progress->state == "completed" || progress->state == "failed")) {
        return;  // Already handled
    }

    // Fulfill promise with exception
    task->fulfill_promise_exception(std::make_exception_ptr(
        PipelineError(PipelineError::EXECUTION_ERROR, "Parent task failed")));

    DFTRACER_UTILS_LOG_DEBUG("Skipped task ID %d ('%s') due to failed parent",
                             task->get_id(), task->get_name().c_str());

    // Recursively skip all children
    // Make a copy to avoid iterator invalidation during recursion
    auto children = task->get_children();
    for (const auto& child : children) {
        if (child) {
            skip_task_and_descendants(child);
        }
    }
}

void Scheduler::start_scheduling_thread() {
    if (scheduling_running_.load()) {
        return;
    }

    scheduling_running_ = true;

    // Create multiple scheduling threads
    scheduling_threads_.reserve(num_scheduling_threads_);
    for (std::size_t i = 0; i < num_scheduling_threads_; ++i) {
        scheduling_threads_.emplace_back(&Scheduler::scheduling_loop, this);
    }

    DFTRACER_UTILS_LOG_DEBUG("Started %zu scheduling threads",
                             num_scheduling_threads_);
}

void Scheduler::stop_scheduling_thread() {
    if (!scheduling_running_.load()) {
        return;
    }

    scheduling_running_ = false;
    ready_cv_.notify_all();

    // Join all scheduling threads
    for (auto& thread : scheduling_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    scheduling_threads_.clear();

    DFTRACER_UTILS_LOG_DEBUG("Stopped %zu scheduling threads",
                             num_scheduling_threads_);
}

void Scheduler::scheduling_loop() {
    DFTRACER_UTILS_LOG_DEBUG("%s", "Scheduling loop started");

    while (scheduling_running_.load()) {
        std::unique_lock<std::mutex> lock(ready_mutex_);

        // Wait for ready tasks or shutdown
        ready_cv_.wait(lock, [this] {
            return !ready_queue_.empty() || !scheduling_running_.load() ||
                   shutdown_requested_.load();
        });

        if (!scheduling_running_.load() || shutdown_requested_.load()) {
            break;
        }

        // Process all ready tasks
        while (!ready_queue_.empty()) {
            auto task = ready_queue_.front();
            ready_queue_.pop();

            lock.unlock();
            process_ready_task(task);
            lock.lock();
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Scheduling loop ended");
}

void Scheduler::process_ready_task(std::shared_ptr<Task> task) {
    if (!task) {
        return;
    }

    // Check if all parents completed
    if (!all_parents_completed(task)) {
        return;  // Not ready yet
    }

    try {
        // Prepare input from parents
        std::any input = prepare_input_for_task(task);

        // Register with watchdog if available
        if (watchdog_) {
            std::chrono::milliseconds timeout = task->has_timeout()
                                                    ? task->get_timeout()
                                                    : default_task_timeout_;

            watchdog_->register_task_start(task->get_id(), task, timeout);
        }

        // Submit to executor
        submit_task_to_executor(task, input);

        DFTRACER_UTILS_LOG_DEBUG("Scheduler submitted task '%s' to executor",
                                 task->get_name().c_str());
    } catch (...) {
        // Error during input preparation (e.g., combiner validation error)
        DFTRACER_UTILS_LOG_ERROR(
            "Failed to prepare input for task ID %d ('%s')", task->get_id(),
            task->get_name().c_str());

        // Store the exception in the task's promise
        task->fulfill_promise_exception(std::current_exception());

        // Handle the error (calls custom handler, checks error policy)
        handle_task_error(task);
    }
}

void Scheduler::request_shutdown() {
    if (shutdown_requested_.load()) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutdown requested for scheduler");
    shutdown_requested_ = true;

    // Request executor shutdown
    executor_->request_shutdown();

    // Wake up waiting threads
    ready_cv_.notify_all();
    done_cv_.notify_all();
}

void Scheduler::set_global_timeout(std::chrono::milliseconds timeout) {
    global_timeout_ = timeout;
    if (watchdog_) {
        watchdog_->set_global_timeout(timeout);
    }
}

void Scheduler::set_default_task_timeout(std::chrono::milliseconds timeout) {
    default_task_timeout_ = timeout;
    if (watchdog_) {
        watchdog_->set_default_task_timeout(timeout);
    }
}

SchedulerMetrics Scheduler::get_metrics() const {
    SchedulerMetrics metrics;

    // Get current time
    auto now = std::chrono::steady_clock::now();

    // Scheduling performance
    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        metrics.ready_queue_depth = ready_queue_.size();
    }
    metrics.total_scheduled = total_scheduled_.load();

    // Count active scheduling threads
    metrics.scheduling_threads_active = 0;
    if (scheduling_running_.load()) {
        // All scheduling threads are active when running
        metrics.scheduling_threads_active = num_scheduling_threads_;
    }

    // Calculate average scheduling latency (simplified for now)
    // TODO: Track actual latencies per task
    metrics.avg_scheduling_latency_ms = 0.0;

    // Pipeline state
    metrics.total_pipeline_tasks = total_tasks_.load();
    metrics.pending_tasks = pending_count_.load();

    // Calculate elapsed time
    if (running_.load() &&
        execution_start_time_.time_since_epoch().count() > 0) {
        metrics.pipeline_elapsed_time_ms =
            std::chrono::duration<double, std::milli>(now -
                                                      execution_start_time_)
                .count();
    } else {
        metrics.pipeline_elapsed_time_ms = 0.0;
    }

    // Error tracking
    if (executor_) {
        auto executor_progress = executor_->get_progress();
        metrics.recent_failures = executor_progress.recent_errors;
    }

    // @todo: Priority distribution (future enhancement)
    // For now, all tasks are NORMAL priority
    metrics.tasks_by_priority[TaskPriority::NORMAL] =
        metrics.total_pipeline_tasks;

    // Timing
    metrics.start_time = execution_start_time_;
    metrics.last_update = now;

    return metrics;
}

ExecutorProgress Scheduler::get_executor_progress() const {
    if (executor_) {
        return executor_->get_progress();
    }
    return ExecutorProgress{};
}

}  // namespace dftracer::utils
