#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/tasks/task.h>

#include <algorithm>
#include <any>
#include <exception>
#include <vector>

namespace dftracer::utils {

Scheduler::Scheduler(Executor* executor) : executor_(executor) {
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

    // Set scheduler reference in executor (for CoroScope)
    executor_->set_scheduler(this);

    // Start executor to ensure workers are ready before scheduling begins
    if (!executor_->is_running()) {
        executor_->start();
    }
}

Scheduler::Scheduler(Executor* executor, const PipelineConfig& config)
    : executor_(executor),
      global_timeout_(config.global_timeout),
      default_task_timeout_(config.default_task_timeout),
      error_policy_(config.error_policy),
      error_handler_(config.error_handler) {
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

    DFTRACER_UTILS_LOG_DEBUG("Scheduler created with watchdog: %s",
                             watchdog_ ? "enabled" : "disabled");
}

Scheduler::~Scheduler() {
    // Shutdown executor first to ensure all worker threads stop before
    // we destroy the scheduler (workers may call scheduler methods)
    if (executor_) {
        executor_->shutdown();
    }

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

    // Submit source task directly to executor -- no scheduling thread needed.
    try {
        std::any src_input;
        if (source->has_initial_input()) {
            src_input = source->get_initial_input().value();
        } else {
            src_input = input;
        }

        if (watchdog_) {
            std::chrono::milliseconds timeout = source->has_timeout()
                                                    ? source->get_timeout()
                                                    : default_task_timeout_;
            watchdog_->register_task_start(source->get_id(), source, timeout);
        }

        submit_task_to_executor(source, src_input);
    } catch (...) {
        DFTRACER_UTILS_LOG_ERROR("Failed to prepare source task ID %d ('%s')",
                                 source->get_id(), source->get_name().c_str());
        source->set_exception(std::current_exception());
        handle_task_error(source);
    }

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
                // Timeout occurred -- release done_mutex_ before calling
                // request_shutdown() so it can lock done_mutex_ to prevent
                // lost notifications on ARM.
                DFTRACER_UTILS_LOG_ERROR("Pipeline timed out after %lld ms",
                                         global_timeout_.count());
                lock.unlock();
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
    if (task->result().has_exception()) {
        task_failed = true;
        has_error_ = true;
        handle_task_error(task);
    }

    // Don't schedule children if shutdown was requested OR if task failed with
    // FAIL_FAST policy.  handle_task_error() already decremented
    // pending_count_, so we must NOT decrement again here.
    if (shutdown_requested_.load() ||
        (task_failed && error_policy_ == ErrorPolicy::FAIL_FAST)) {
        DFTRACER_UTILS_LOG_DEBUG(
            "Skipping child scheduling for task '%s' due to shutdown or "
            "FAIL_FAST",
            task->get_name().c_str());
        return;
    }

    // Make a copy of children to avoid iterator invalidation or use-after-free
    // if task is destroyed during iteration
    auto children = task->get_children();

    // Schedule ready children by submitting directly to executor.
    // Even if this task failed with CONTINUE/CUSTOM policy, we still
    // process children normally. When a child becomes ready, we check if any
    // parent failed and skip it at that point.
    for (const auto& child : children) {
        if (!child) {
            continue;  // Skip null children
        }

        // Atomic decrement returns previous value.  The child becomes
        // ready exactly when prev == 1 (transition from 1 → 0).
        int prev = child->decrement_pending_parents();

        if (prev == 1) {
            // For CONTINUE/CUSTOM policy: skip children if any parent failed
            bool has_failed_parent = false;
            if (error_policy_ != ErrorPolicy::FAIL_FAST) {
                for (const auto& parent : child->get_parents()) {
                    if (parent->result().has_exception()) {
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

            // Submit directly to executor -- fully event-driven, no
            // scheduling thread.
            try {
                std::any input = prepare_input_for_task(child);

                // Register with watchdog if available
                if (watchdog_) {
                    std::chrono::milliseconds timeout =
                        child->has_timeout() ? child->get_timeout()
                                             : default_task_timeout_;
                    watchdog_->register_task_start(child->get_id(), child,
                                                   timeout);
                }

                submit_task_to_executor(child, input);

                DFTRACER_UTILS_LOG_DEBUG(
                    "Directly submitted child task '%s' to executor",
                    child->get_name().c_str());
            } catch (...) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to prepare input for child task ID %d "
                    "('%s')",
                    child->get_id(), child->get_name().c_str());

                child->set_exception(std::current_exception());
                handle_task_error(child);
            }
        }
    }

    // Report progress using executor's metrics
    // NOTE: Must be called BEFORE decrementing pending_count_ and notify_all()
    // to avoid race condition where schedule() returns before progress callback
    // is invoked for the last task
    if (progress_callback_) {
        auto executor_progress = executor_->get_progress();
        progress_callback_(executor_progress.tasks_completed, total_tasks_);
    }

    // Invoke coroutine completion callbacks (resume awaiting coroutines)
    invoke_completion_callbacks(task->get_id());

    // Decrement pending count
    // Only if not already decremented by handle_task_error
    if (!task_failed) {
        --pending_count_;
        if (pending_count_ == 0) {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_cv_.notify_all();
        }
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
        // Parent is guaranteed complete (child only processed when all
        // parents done). Use get_ready() -- non-blocking.
        auto value = parents[0]->result().get_ready();
        parents[0]->result().release_reader();
        return value;
    }

    // Single parent with combiner OR multiple parents
    // check if task has custom combiner
    if (task->has_combiner()) {
        std::vector<std::any> parent_outputs;
        parent_outputs.reserve(parents.size());
        for (const auto& parent : parents) {
            parent_outputs.push_back(parent->result().get_ready());
            parent->result().release_reader();
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

    // Pack parent outputs into a vector for type-safe multi-arg functions
    // The Task's wrap_function will handle unpacking into typed arguments
    std::vector<std::any> parent_outputs;
    parent_outputs.reserve(parents.size());
    for (const auto& parent : parents) {
        parent_outputs.push_back(parent->result().get_ready());
        parent->result().release_reader();
    }

    // Return the vector - wrap_function will unpack it into the typed tuple
    return std::any(parent_outputs);
}

void Scheduler::submit_task_to_executor(std::shared_ptr<Task> task,
                                        const std::any& input) {
    DFTRACER_UTILS_LOG_DEBUG("Submitting task ID %ld ('%s') to executor",
                             task->get_id(), task->get_name().c_str());

    // @Note: pending_count_ is incremented before this call,
    // not here, to avoid race condition with wait predicate

    auto input_ptr = std::make_shared<std::any>(std::move(input));

    // Determine parent task ID for tracking
    TaskIndex parent_id = -1;
    auto* worker_context =
        static_cast<Executor::WorkerContext*>(get_current_worker_context());
    if (worker_context) {
        parent_id = worker_context->current_task_id.load();
    }

    // Submit via Coro-based path
    executor_->submit_task(task, input_ptr, parent_id);
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
            auto ex = task->result().get_exception();
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
                // std::any works as a wildcard in both directions:
                // - A task expecting std::any can accept any input type
                // - A task outputting std::any can connect to any input type
                // (runtime cast)
                bool types_match =
                    (parent->get_output_type() == current->get_input_type()) ||
                    (current->get_input_type() == typeid(std::any)) ||
                    (parent->get_output_type() == typeid(std::any));

                if (!types_match) {
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

    // Check if already processed -- direct Task check avoids registry lock
    if (task->is_completed()) {
        return;  // Already handled
    }

    // Set exception on skipped task
    task->set_exception(std::make_exception_ptr(
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

void Scheduler::request_shutdown() {
    if (shutdown_requested_.load()) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutdown requested for scheduler");
    shutdown_requested_ = true;

    // Request executor shutdown
    executor_->request_shutdown();

    // Wake up waiting thread -- lock-then-unlock done_mutex_ before
    // notifying to prevent lost notifications on ARM where release/acquire
    // on separate variables doesn't provide total ordering.
    {
        std::lock_guard<std::mutex> lock(done_mutex_);
    }
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

// ============================================================================
// Coroutine Support - Completion Callbacks
// ============================================================================

void Scheduler::register_task_completion_callback(
    TaskIndex task_id, std::function<void()> callback) {
    completion_callbacks_.with_shard(task_id, [&](CallbackMap& callbacks) {
        callbacks[task_id].push_back(std::move(callback));
    });
}

void Scheduler::invoke_completion_callbacks(TaskIndex task_id) {
    // Only locks the specific shard for this task_id
    // Other tasks completing concurrently don't block
    completion_callbacks_.with_shard(task_id, [&](CallbackMap& callbacks) {
        if (auto it = callbacks.find(task_id); it != callbacks.end()) {
            // Invoke all registered callbacks
            for (auto& callback : it->second) {
                try {
                    callback();  // Resume awaiting coroutines
                } catch (const std::exception& e) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Exception in completion callback for task ID %d: %s",
                        task_id, e.what());
                } catch (...) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Unknown exception in completion callback for task ID "
                        "%d",
                        task_id);
                }
            }
            // Clean up callbacks after invocation
            callbacks.erase(it);
        }
    });
}

}  // namespace dftracer::utils
