// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/pipeline/scheduler.h>
#include <dftracer/utils/runtime/scheduler/watchdog.h>
#include <dftracer/utils/runtime/tasks/noop_task.h>
#include <dftracer/utils/runtime/tasks/task.h>

#include <any>

namespace dftracer::utils {

Pipeline::Pipeline(const PipelineConfig& config)
    : name_(config.name),
      error_policy_(config.error_policy),
      error_handler_(config.error_handler) {
    // The settings this pipeline asks for apply to the process runtime. One
    // it leaves unset keeps what the runtime has.
    RuntimeSettings settings;
    settings.threads = config.executor_threads;
    settings.io_threads = config.io_thread_count;
    if (config.min_workers) settings.eager = *config.min_workers == 0;
    settings.deadlock_timeout = config.executor_deadlock_timeout;
    settings.io_backend = config.io_backend_type;
    settings.io_batch_threshold = config.io_batch_threshold;
    configure_default_runtime(settings);
    runtime_ = default_runtime_shared();

    if (config.enable_watchdog) {
        watchdog_ = std::make_unique<Watchdog>(
            config.watchdog_interval, config.global_timeout,
            config.default_task_timeout, config.long_task_warning_threshold);
        watchdog_->set_executor(runtime_->executor());
    }
    scheduler_ = std::make_unique<Scheduler>(runtime_->executor(),
                                             watchdog_.get(), config);

    DFTRACER_UTILS_LOG_DEBUG(
        "Pipeline '%s' created with config: executor_threads=%zu, "
        "watchdog=%s",
        name_.c_str(), config.executor_threads,
        config.enable_watchdog ? "enabled" : "disabled");
}

Pipeline::~Pipeline() {
    DFTRACER_UTILS_LOG_DEBUG("Pipeline '%s' destroyed", name_.c_str());
    scheduler_.reset();
    if (watchdog_) watchdog_->stop();
}

void Pipeline::set_source(dstd::SharedPtr<Task> source) {
    if (!source) {
        throw PipelineError(PipelineError::ValidationError,
                            "Source task cannot be null");
    }
    source_ = source;
    validated_ = false;
}

void Pipeline::set_destination(dstd::SharedPtr<Task> destination) {
    destination_ = destination;
    validated_ = false;
}

void Pipeline::set_source(
    std::initializer_list<dstd::SharedPtr<Task>> sources) {
    if (sources.size() == 0) {
        throw PipelineError(PipelineError::ValidationError,
                            "Cannot set zero sources");
    }

    if (sources.size() == 1) {
        set_source(*sources.begin());
        return;
    }

    auto noop = make_noop_task("__start__");
    for (auto& source : sources) {
        if (!source) {
            throw PipelineError(PipelineError::ValidationError,
                                "Source task cannot be null");
        }
        source->depends_on(noop);
    }

    source_ = noop;
    validated_ = false;
}

void Pipeline::set_destination(
    std::initializer_list<dstd::SharedPtr<Task>> destinations) {
    if (destinations.size() == 0) {
        throw PipelineError(PipelineError::ValidationError,
                            "Cannot set zero destinations");
    }

    if (destinations.size() == 1) {
        set_destination(*destinations.begin());
        return;
    }

    auto noop = make_noop_task("__end__");
    for (auto& dest : destinations) {
        if (!dest) {
            throw PipelineError(PipelineError::ValidationError,
                                "Destination task cannot be null");
        }
        noop->depends_on(dest);
    }

    destination_ = noop;
    validated_ = false;
}

void Pipeline::set_source(const std::vector<dstd::SharedPtr<Task>>& sources) {
    if (sources.empty()) {
        throw PipelineError(PipelineError::ValidationError,
                            "Cannot set zero sources");
    }

    if (sources.size() == 1) {
        set_source(sources[0]);
        return;
    }

    auto noop = make_noop_task("__start__");
    for (const auto& source : sources) {
        if (!source) {
            throw PipelineError(PipelineError::ValidationError,
                                "Source task cannot be null");
        }
        source->depends_on(noop);
    }

    source_ = noop;
    validated_ = false;
}

void Pipeline::set_destination(
    const std::vector<dstd::SharedPtr<Task>>& destinations) {
    if (destinations.empty()) {
        throw PipelineError(PipelineError::ValidationError,
                            "Cannot set zero destinations");
    }

    if (destinations.size() == 1) {
        set_destination(destinations[0]);
        return;
    }

    auto noop = make_noop_task("__end__");
    for (const auto& dest : destinations) {
        if (!dest) {
            throw PipelineError(PipelineError::ValidationError,
                                "Destination task cannot be null");
        }
        noop->depends_on(dest);
    }

    destination_ = noop;
    validated_ = false;
}

bool Pipeline::validate() {
    if (!source_) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Pipeline validation failed: no source task");
        return false;
    }

    collect_all_tasks();

    if (has_cycles()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Pipeline validation failed: cycle detected");
        return false;
    }

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
    if (!validated_) {
        if (!validate()) {
            throw PipelineError(PipelineError::ValidationError,
                                "Pipeline validation failed");
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Executing pipeline '%s'", name_.c_str());

    scheduler_->set_error_policy(error_policy_);
    if (error_handler_) {
        scheduler_->set_error_handler(error_handler_);
    }

    std::any wrapped_input = input;

    scheduler_->schedule(source_, wrapped_input);

    PipelineOutput output;

    if (destination_) {
        try {
            output[destination_->id()] = destination_->result().get();
        } catch (...) {
            if (error_policy_ == ErrorPolicy::FailFast) {
                throw;
            }
        }
    } else {
        for (const auto& task : all_tasks_) {
            if (task->children().empty()) {
                try {
                    output[task->id()] = task->result().get();
                } catch (...) {
                    if (error_policy_ == ErrorPolicy::FailFast) {
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
    std::unordered_set<common::TaskIndex> visited;
    collect_tasks_dfs(source_, visited);
}

void Pipeline::collect_tasks_dfs(
    dstd::SharedPtr<Task> task,
    std::unordered_set<common::TaskIndex>& visited) {
    if (!task || visited.find(task->id()) != visited.end()) {
        return;
    }

    visited.insert(task->id());
    all_tasks_.push_back(task);

    for (const auto& child : task->children()) {
        collect_tasks_dfs(child, visited);
    }
}

bool Pipeline::validate_reachability() {
    if (!source_ || !destination_) {
        return false;
    }

    std::unordered_set<common::TaskIndex> visited;
    return is_reachable_dfs(source_, destination_, visited);
}

bool Pipeline::is_reachable_dfs(
    dstd::SharedPtr<Task> current, dstd::SharedPtr<Task> target,
    std::unordered_set<common::TaskIndex>& visited) {
    if (!current) {
        return false;
    }

    if (current->id() == target->id()) {
        return true;
    }

    if (visited.find(current->id()) != visited.end()) {
        return false;
    }

    visited.insert(current->id());

    for (const auto& child : current->children()) {
        if (is_reachable_dfs(child, target, visited)) {
            return true;
        }
    }

    return false;
}

bool Pipeline::has_cycles() {
    std::unordered_set<common::TaskIndex> visited;
    std::unordered_set<common::TaskIndex> rec_stack;

    for (const auto& task : all_tasks_) {
        if (visited.find(task->id()) == visited.end()) {
            if (has_cycles_dfs(task, visited, rec_stack)) {
                return true;
            }
        }
    }

    return false;
}

bool Pipeline::has_cycles_dfs(
    dstd::SharedPtr<Task> task, std::unordered_set<common::TaskIndex>& visited,
    std::unordered_set<common::TaskIndex>& rec_stack) {
    if (!task) {
        return false;
    }

    common::TaskIndex id = task->id();

    if (rec_stack.find(id) != rec_stack.end()) {
        DFTRACER_UTILS_LOG_ERROR("Cycle detected at task '%s'", task->name());
        return true;
    }

    if (visited.find(id) != visited.end()) {
        return false;
    }

    visited.insert(id);
    rec_stack.insert(id);

    for (const auto& child : task->children()) {
        if (has_cycles_dfs(child, visited, rec_stack)) {
            return true;
        }
    }

    rec_stack.erase(id);
    return false;
}

}  // namespace dftracer::utils
