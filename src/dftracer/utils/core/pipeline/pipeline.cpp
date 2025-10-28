#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/noop_task.h>
#include <dftracer/utils/core/tasks/task.h>

#include <algorithm>
#include <sstream>

namespace dftracer::utils {

Pipeline::Pipeline(const PipelineConfigManager& config)
    : name_(config.name),
      executor_threads_(config.executor_threads),
      error_policy_(config.error_policy),
      error_handler_(config.error_handler) {
    // Create executor with configured threads
    executor_ = std::make_unique<Executor>(config.executor_threads);

    // Create scheduler with reference to executor and full config
    scheduler_ = std::make_unique<Scheduler>(executor_.get(), config);

    DFTRACER_UTILS_LOG_DEBUG(
        "Pipeline '%s' created with config: executor_threads=%zu, "
        "scheduler_threads=%zu, watchdog=%s",
        name_.c_str(), config.executor_threads, config.scheduler_threads,
        config.enable_watchdog ? "enabled" : "disabled");
}

Pipeline::~Pipeline() {
    DFTRACER_UTILS_LOG_DEBUG("Pipeline '%s' destroyed", name_.c_str());
    // Ensure executor is properly shut down before destruction
    if (executor_) {
        executor_->shutdown();
    }
}

void Pipeline::set_source(std::shared_ptr<Task> source) {
    if (!source) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Source task cannot be null");
    }
    source_ = source;
    validated_ = false;  // Need to revalidate
}

void Pipeline::set_destination(std::shared_ptr<Task> destination) {
    destination_ = destination;
    validated_ = false;  // Need to revalidate
}

void Pipeline::add_sources(
    std::initializer_list<std::shared_ptr<Task>> sources) {
    if (sources.size() == 0) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Cannot add zero sources");
    }

    if (sources.size() == 1) {
        // Single source - no need for NoOpTask
        set_source(*sources.begin());
        return;
    }

    // Multiple sources - create NoOpTask
    auto noop = make_noop_task();

    // Connect all sources as children of noop
    for (auto& source : sources) {
        if (!source) {
            throw PipelineError(PipelineError::VALIDATION_ERROR,
                                "Source task cannot be null");
        }
        source->depends_on(noop);
    }

    source_ = noop;
    validated_ = false;
}

bool Pipeline::validate() {
    if (!source_) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Pipeline validation failed: no source task");
        return false;
    }

    // Collect all tasks
    collect_all_tasks();

    // Check for cycles
    if (has_cycles()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Pipeline validation failed: cycle detected");
        return false;
    }

    // If destination is set, check reachability
    if (destination_ && !validate_reachability()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Pipeline validation failed: destination not "
                                 "reachable from source");
        return false;
    }

    validated_ = true;
    DFTRACER_UTILS_LOG_DEBUG("Pipeline '%s' validated successfully (%zu tasks)",
                             name_.c_str(), all_tasks_.size());
    return true;
}

PipelineOutput Pipeline::execute(const std::any& input) {
    // Validate if not already done
    if (!validated_) {
        if (!validate()) {
            throw PipelineError(PipelineError::VALIDATION_ERROR,
                                "Pipeline validation failed");
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Executing pipeline '%s'", name_.c_str());

    // Set error policy and handler
    scheduler_->set_error_policy(error_policy_);
    if (error_handler_) {
        scheduler_->set_error_handler(error_handler_);
    }

    // Wrap input in std::any if not already wrapped
    std::any wrapped_input = input;

    // Execute via scheduler
    scheduler_->schedule(source_, wrapped_input);

    // Extract results
    PipelineOutput output;

    if (destination_) {
        // Single destination
        try {
            auto future = destination_->get_future();
            output[destination_->get_id()] = future.get();
        } catch (...) {
            // If error policy is FAIL_FAST, rethrow
            // Otherwise (CONTINUE/CUSTOM), skip failed destination
            if (error_policy_ == ErrorPolicy::FAIL_FAST) {
                throw;
            }
        }
    } else {
        // All terminal tasks (tasks with no children)
        for (const auto& task : all_tasks_) {
            if (task->get_children().empty()) {
                try {
                    auto future = task->get_future();
                    output[task->get_id()] = future.get();
                } catch (...) {
                    // If error policy is FAIL_FAST, rethrow
                    // Otherwise (CONTINUE/CUSTOM), skip failed task
                    if (error_policy_ == ErrorPolicy::FAIL_FAST) {
                        throw;
                    }
                }
            }
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Pipeline '%s' execution complete", name_.c_str());
    return output;
}

void Pipeline::set_error_policy(ErrorPolicy policy) { error_policy_ = policy; }

void Pipeline::set_progress_callback(
    std::function<void(size_t completed, size_t total)> callback) {
    scheduler_->set_progress_callback(std::move(callback));
}

void Pipeline::collect_all_tasks() {
    all_tasks_.clear();
    std::unordered_set<TaskIndex> visited;
    collect_tasks_dfs(source_, visited);
}

void Pipeline::collect_tasks_dfs(std::shared_ptr<Task> task,
                                 std::unordered_set<TaskIndex>& visited) {
    if (!task || visited.find(task->get_id()) != visited.end()) {
        return;
    }

    visited.insert(task->get_id());
    all_tasks_.push_back(task);

    for (const auto& child : task->get_children()) {
        collect_tasks_dfs(child, visited);
    }
}

bool Pipeline::validate_reachability() {
    if (!source_ || !destination_) {
        return false;
    }

    std::unordered_set<TaskIndex> visited;
    return is_reachable_dfs(source_, destination_, visited);
}

bool Pipeline::is_reachable_dfs(std::shared_ptr<Task> current,
                                std::shared_ptr<Task> target,
                                std::unordered_set<TaskIndex>& visited) {
    if (!current) {
        return false;
    }

    if (current->get_id() == target->get_id()) {
        return true;
    }

    if (visited.find(current->get_id()) != visited.end()) {
        return false;
    }

    visited.insert(current->get_id());

    for (const auto& child : current->get_children()) {
        if (is_reachable_dfs(child, target, visited)) {
            return true;
        }
    }

    return false;
}

bool Pipeline::validate_types() {
    // Type validation happens during DAG construction in Task::depends_on()
    // Here we just do a sanity check

    for (const auto& task : all_tasks_) {
        for (const auto& parent : task->get_parents()) {
            // Skip validation for void outputs (synchronization only)
            if (parent->get_output_type() == typeid(void)) {
                continue;
            }

            // For single parent, types should match (unless task has custom
            // combiner)
            if (task->get_parents().size() == 1 && !task->has_combiner()) {
                if (parent->get_output_type() != task->get_input_type()) {
                    std::ostringstream oss;
                    oss << "Type mismatch: task '" << task->get_name()
                        << "' expects " << task->get_input_type().name()
                        << " but parent '" << parent->get_name() << "' outputs "
                        << parent->get_output_type().name();
                    DFTRACER_UTILS_LOG_ERROR("%s", oss.str().c_str());
                    return false;
                }
            }
            // Multiple parents are handled via combiner or tuple packing
        }
    }

    return true;
}

bool Pipeline::has_cycles() {
    std::unordered_set<TaskIndex> visited;
    std::unordered_set<TaskIndex> rec_stack;

    for (const auto& task : all_tasks_) {
        if (visited.find(task->get_id()) == visited.end()) {
            if (has_cycles_dfs(task, visited, rec_stack)) {
                return true;
            }
        }
    }

    return false;
}

bool Pipeline::has_cycles_dfs(std::shared_ptr<Task> task,
                              std::unordered_set<TaskIndex>& visited,
                              std::unordered_set<TaskIndex>& rec_stack) {
    if (!task) {
        return false;
    }

    TaskIndex id = task->get_id();

    if (rec_stack.find(id) != rec_stack.end()) {
        // Found cycle
        DFTRACER_UTILS_LOG_ERROR("Cycle detected at task '%s'",
                                 task->get_name().c_str());
        return true;
    }

    if (visited.find(id) != visited.end()) {
        // Already processed
        return false;
    }

    visited.insert(id);
    rec_stack.insert(id);

    for (const auto& child : task->get_children()) {
        if (has_cycles_dfs(child, visited, rec_stack)) {
            return true;
        }
    }

    rec_stack.erase(id);
    return false;
}

}  // namespace dftracer::utils
