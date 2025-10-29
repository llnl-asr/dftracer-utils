#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/executor_context.h>
#include <dftracer/utils/core/pipeline/pipeline.h>

#include <chrono>
#include <thread>

namespace dftracer::utils {
Task* ExecutorContext::get_task(TaskIndex index) const {
    if (index < static_cast<TaskIndex>(pipeline_->size())) {
        return pipeline_->get_task(index);
    } else {
        return get_dynamic_task(index);
    }
}

const std::vector<TaskIndex>& ExecutorContext::get_task_dependencies(
    TaskIndex index) const {
    if (index < static_cast<TaskIndex>(pipeline_->size())) {
        return pipeline_->get_task_dependencies(index);
    } else {
        return get_dynamic_dependencies(index);
    }
}

const std::vector<TaskIndex>& ExecutorContext::get_task_dependents(
    TaskIndex index) const {
    if (index < static_cast<TaskIndex>(pipeline_->size())) {
        return pipeline_->get_task_dependents(index);
    } else {
        return get_dynamic_dependents(index);
    }
}

TaskIndex ExecutorContext::add_dynamic_task(std::unique_ptr<Task> task,
                                            TaskIndex depends_on) {
    TaskIndex task_id =
        static_cast<TaskIndex>(pipeline_->size() + dynamic_tasks_.size());

    dynamic_tasks_.push_back(std::move(task));

    while (dynamic_dependencies_.size() <=
           static_cast<size_t>(task_id - pipeline_->size())) {
        dynamic_dependencies_.emplace_back();
        dynamic_dependents_.emplace_back();
    }

    if (depends_on >= 0) {
        add_dynamic_dependency(depends_on, task_id);
    }

    {
        std::lock_guard<std::mutex> lock(task_completed_mutex_);
        task_completed_[task_id] = false;
    }
    {
        std::lock_guard<std::mutex> lock(dependency_count_mutex_);
        dependency_count_[task_id] = (depends_on >= 0) ? 1 : 0;
    }

    return task_id;
}

void ExecutorContext::add_dynamic_dependency(TaskIndex from, TaskIndex to) {
    if (from < static_cast<TaskIndex>(pipeline_->size()) &&
        to < static_cast<TaskIndex>(pipeline_->size())) {
        return;
    }

    std::size_t from_idx =
        (from >= static_cast<TaskIndex>(pipeline_->size()))
            ? static_cast<std::size_t>(
                  from - static_cast<TaskIndex>(pipeline_->size()))
            : 0;
    std::size_t to_idx =
        (to >= static_cast<TaskIndex>(pipeline_->size()))
            ? static_cast<std::size_t>(
                  to - static_cast<TaskIndex>(pipeline_->size()))
            : 0;

    if (from >= static_cast<TaskIndex>(pipeline_->size())) {
        while (dynamic_dependents_.size() <= from_idx) {
            dynamic_dependents_.emplace_back();
        }
        dynamic_dependents_[from_idx].push_back(to);
    }

    if (to >= static_cast<TaskIndex>(pipeline_->size())) {
        while (dynamic_dependencies_.size() <= to_idx) {
            dynamic_dependencies_.emplace_back();
        }
        dynamic_dependencies_[to_idx].push_back(from);
    }

    {
        std::lock_guard<std::mutex> lock(dependency_count_mutex_);
        dependency_count_[to]++;
    }
}

Task* ExecutorContext::get_dynamic_task(TaskIndex index) const {
    if (index < static_cast<TaskIndex>(pipeline_->size())) return nullptr;

    size_t task_idx = static_cast<size_t>(index - pipeline_->size());
    if (task_idx >= dynamic_tasks_.size()) return nullptr;

    return dynamic_tasks_[task_idx].get();
}

const std::vector<TaskIndex>& ExecutorContext::get_dynamic_dependencies(
    TaskIndex index) const {
    static const std::vector<TaskIndex> empty;
    if (index < static_cast<TaskIndex>(pipeline_->size())) return empty;

    size_t task_idx = static_cast<size_t>(index - pipeline_->size());
    if (task_idx >= dynamic_dependencies_.size()) return empty;

    return dynamic_dependencies_[task_idx];
}

const std::vector<TaskIndex>& ExecutorContext::get_dynamic_dependents(
    TaskIndex index) const {
    static const std::vector<TaskIndex> empty;
    if (index < static_cast<TaskIndex>(pipeline_->size())) return empty;

    size_t task_idx = static_cast<size_t>(index - pipeline_->size());
    if (task_idx >= dynamic_dependents_.size()) return empty;

    return dynamic_dependents_[task_idx];
}

void ExecutorContext::set_task_output(TaskIndex index, std::any output) {
    auto task_output = std::make_unique<ExecutorTaskOutput>();
    task_output->data = std::move(output);

    const auto& dependents = get_task_dependents(index);
    task_output->dependency_refs = static_cast<int>(dependents.size());

    task_output->user_refs = 0;

    std::lock_guard<std::mutex> lock(task_outputs_mutex_);
    task_outputs_[index] = std::move(task_output);
}

std::any ExecutorContext::get_task_output(TaskIndex index) const {
    std::lock_guard<std::mutex> lock(task_outputs_mutex_);
    auto it = task_outputs_.find(index);
    return (it != task_outputs_.end()) ? it->second->data : std::any{};
}

void ExecutorContext::set_task_completed(TaskIndex index, bool completed) {
    std::lock_guard<std::mutex> lock(task_completed_mutex_);
    task_completed_[index] = completed;
}

bool ExecutorContext::is_task_completed(TaskIndex index) const {
    std::lock_guard<std::mutex> lock(task_completed_mutex_);
    auto it = task_completed_.find(index);
    return (it != task_completed_.end()) ? it->second : false;
}

void ExecutorContext::reset() {
    // Clear all dynamic state
    dynamic_tasks_.clear();
    dynamic_dependencies_.clear();
    dynamic_dependents_.clear();

    // Clear all execution state
    {
        std::lock_guard<std::mutex> lock(task_outputs_mutex_);
        task_outputs_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(task_completed_mutex_);
        task_completed_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(dependency_count_mutex_);
        dependency_count_.clear();
    }
}

void ExecutorContext::initialize_task_tracking() {
    std::unordered_map<TaskIndex, bool> temp_task_completed;
    temp_task_completed.reserve(pipeline_->size());
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(pipeline_->size()); ++i) {
        temp_task_completed[i] = false;
    }

    {
        std::lock_guard<std::mutex> lock(task_completed_mutex_);
        task_completed_ = std::move(temp_task_completed);
    }
}

bool ExecutorContext::validate() const {
    if (is_empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Pipeline is empty");
        return false;
    }

    if (has_cycles()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Pipeline contains cycles");
        return false;
    }

    for (TaskIndex i = 0; i < static_cast<TaskIndex>(pipeline_->size()); ++i) {
        const auto& task_dependencies = get_task_dependencies(i);

        if (task_dependencies.empty()) {
            continue;
        } else if (task_dependencies.size() == 1) {
            TaskIndex dep = task_dependencies[0];
            Task* dep_task = get_task(dep);
            Task* current_task = get_task(i);

            if (dep_task->get_output_type() != current_task->get_input_type()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Type mismatch between task %d (output: %s) and task %d "
                    "(expected input: %s)",
                    dep, dep_task->get_output_type().name(), i,
                    current_task->get_input_type().name());
                return false;
            }
        } else {
            Task* current_task = get_task(i);
            if (current_task->get_input_type() !=
                typeid(std::vector<std::any>)) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Task %d has %zu dependencies but expects input type %s "
                    "instead of std::vector<std::any>",
                    i, task_dependencies.size(),
                    current_task->get_input_type().name());
                return false;
            }
        }
    }
    return true;
}

bool ExecutorContext::is_empty() const { return pipeline_->empty(); }

bool ExecutorContext::has_cycles() const { return pipeline_->has_cycles(); }

bool ExecutorContext::is_terminal_task(TaskIndex index) const {
    return index < static_cast<TaskIndex>(pipeline_->size());
}

void ExecutorContext::increment_user_ref(TaskIndex index) {
    std::lock_guard<std::mutex> lock(task_outputs_mutex_);
    auto it = task_outputs_.find(index);
    if (it != task_outputs_.end()) {
        it->second->user_refs.fetch_add(1);
    }
}

void ExecutorContext::release_user_ref(TaskIndex index) {
    std::lock_guard<std::mutex> lock(task_outputs_mutex_);
    auto it = task_outputs_.find(index);
    if (it != task_outputs_.end()) {
        it->second->user_refs.fetch_sub(1);
        if (it->second->can_cleanup()) {
            task_outputs_.erase(index);
        }
    }
}

std::any ExecutorContext::consume_task_output(TaskIndex index) {
    std::lock_guard<std::mutex> lock(task_outputs_mutex_);
    auto it = task_outputs_.find(index);
    if (it == task_outputs_.end()) {
        return std::any{};
    }

    std::any result = it->second->data;

    it->second->dependency_refs.fetch_sub(1);

    if (it->second->can_cleanup()) {
        task_outputs_.erase(index);
    }

    return result;
}

void ExecutorContext::mark_task_completed(TaskIndex index) {
    set_task_completed(index, true);
}

bool ExecutorContext::wait_for_task_completion(TaskIndex index) {
    while (!is_task_completed(index)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void ExecutorContext::set_promise_fulfiller(
    TaskIndex index, std::function<void(const std::any&)> fulfiller) {
    std::lock_guard<std::mutex> lock(promise_fulfillers_mutex_);
    promise_fulfillers_[index] = std::move(fulfiller);
}

void ExecutorContext::set_promise_exception_fulfiller(
    TaskIndex index, std::function<void(std::exception_ptr)> fulfiller) {
    std::lock_guard<std::mutex> lock(promise_fulfillers_mutex_);
    promise_exception_fulfillers_[index] = std::move(fulfiller);
}

void ExecutorContext::fulfill_dynamic_promise(TaskIndex index,
                                              const std::any& result) const {
    std::function<void(const std::any&)> fulfiller;
    {
        std::lock_guard<std::mutex> lock(promise_fulfillers_mutex_);
        auto it = promise_fulfillers_.find(index);
        if (it != promise_fulfillers_.end()) {
            fulfiller = it->second;
        }
    }
    if (fulfiller) {
        fulfiller(result);
    }
}

void ExecutorContext::fulfill_dynamic_promise_exception(
    TaskIndex index, std::exception_ptr exception) const {
    std::function<void(std::exception_ptr)> fulfiller;
    {
        std::lock_guard<std::mutex> lock(promise_fulfillers_mutex_);
        auto it = promise_exception_fulfillers_.find(index);
        if (it != promise_exception_fulfillers_.end()) {
            fulfiller = it->second;
        }
    }
    if (fulfiller) {
        fulfiller(exception);
    }
}

}  // namespace dftracer::utils
