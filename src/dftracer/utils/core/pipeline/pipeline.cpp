#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/tasks/noop_task.h>
#include <dftracer/utils/core/tasks/task.h>

#include <any>

namespace dftracer::utils {

Pipeline::Pipeline(const PipelineConfig& config)
    : name_(config.name),
      error_policy_(config.error_policy),
      error_handler_(config.error_handler) {
    ExecutorConfig exec_cfg;
    exec_cfg.num_threads = config.executor_threads;
    // Explicit config wins; unset resolves from the env default (elastic unless
    // DFTRACER_UTILS_ELASTIC is off), so tests can pin either path.
    exec_cfg.min_workers = config.min_workers.has_value()
                               ? *config.min_workers
                               : (elastic_default_enabled() ? 1 : 0);
    exec_cfg.elastic_keepalive = config.elastic_keepalive;
    exec_cfg.idle_timeout = config.executor_idle_timeout;
    exec_cfg.deadlock_timeout = config.executor_deadlock_timeout;
    exec_cfg.io_pool_size = config.io_thread_count;
    exec_cfg.io_backend_type = config.io_backend_type;
    exec_cfg.io_batch_threshold = config.io_batch_threshold;

    std::unique_ptr<Watchdog> watchdog;
    if (config.enable_watchdog) {
        watchdog = std::make_unique<Watchdog>(
            config.watchdog_interval, config.global_timeout,
            config.default_task_timeout, config.long_task_warning_threshold);
    }

    runtime_ = std::make_unique<Runtime>(exec_cfg, std::move(watchdog));
    scheduler_ = std::make_unique<Scheduler>(runtime_->executor(),
                                             runtime_->watchdog(), config);

    DFTRACER_UTILS_LOG_DEBUG(
        "Pipeline '%s' created with config: executor_threads=%zu, "
        "watchdog=%s",
        name_.c_str(), config.executor_threads,
        config.enable_watchdog ? "enabled" : "disabled");
}

Pipeline::~Pipeline() {
    DFTRACER_UTILS_LOG_DEBUG("Pipeline '%s' destroyed", name_.c_str());
    runtime_->shutdown();
}

void Pipeline::set_source(std::shared_ptr<Task> source) {
    if (!source) {
        throw PipelineError(PipelineError::ValidationError,
                            "Source task cannot be null");
    }
    source_ = source;
    validated_ = false;
}

void Pipeline::set_destination(std::shared_ptr<Task> destination) {
    destination_ = destination;
    validated_ = false;
}

void Pipeline::set_source(
    std::initializer_list<std::shared_ptr<Task>> sources) {
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
    std::initializer_list<std::shared_ptr<Task>> destinations) {
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

void Pipeline::set_source(const std::vector<std::shared_ptr<Task>>& sources) {
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
    const std::vector<std::shared_ptr<Task>>& destinations) {
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
            output[destination_->get_id()] = destination_->result().get();
        } catch (...) {
            if (error_policy_ == ErrorPolicy::FailFast) {
                throw;
            }
        }
    } else {
        for (const auto& task : all_tasks_) {
            if (task->get_children().empty()) {
                try {
                    output[task->get_id()] = task->result().get();
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
        DFTRACER_UTILS_LOG_ERROR("Cycle detected at task '%s'",
                                 task->get_name());
        return true;
    }

    if (visited.find(id) != visited.end()) {
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
