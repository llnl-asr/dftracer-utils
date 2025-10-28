#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <algorithm>
#include <any>
#include <optional>
#include <sstream>

namespace dftracer::utils {

std::shared_ptr<Task> Task::depends_on(std::shared_ptr<Task> parent) {
    if (!parent) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Cannot add null parent to task");
    }

    // Add parent to this task
    parents_.push_back(parent);

    // Add this task as child to parent
    parent->add_child(shared_from_this());

    return shared_from_this();
}

std::shared_ptr<Task> Task::depends_on(
    std::initializer_list<std::shared_ptr<Task>> parent_list) {
    // Simply delegate to single-parent version for each parent
    for (auto& parent : parent_list) {
        depends_on(parent);
    }
    return shared_from_this();
}

std::shared_ptr<Task> Task::with_combiner(
    std::function<std::any(const std::vector<std::any>&)> combiner) {
    input_combiner_ = std::move(combiner);
    has_custom_combiner_ = true;
    return shared_from_this();
}

std::shared_ptr<Task> Task::with_name(std::string name) {
    name_ = std::move(name);
    return shared_from_this();
}

std::any Task::execute(TaskContext& context, const std::any& input) {
    try {
        std::any result = func_(context, input);
        return result;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Task '%s' execution failed: %s",
                                 name_.c_str(), e.what());
        throw;
    }
}

void Task::fulfill_promise(std::any result) {
    if (!promise_) {
        DFTRACER_UTILS_LOG_ERROR("Task '%s': promise is null!", name_.c_str());
        throw std::runtime_error("Promise is null");
    }

    try {
        promise_->set_value(std::move(result));
        completed_ = true;
    } catch (const std::future_error& e) {
        // Promise already set or no associated state
        DFTRACER_UTILS_LOG_ERROR("Task '%s' promise error: %s (code: %d)",
                                 name_.c_str(), e.what(), e.code().value());
        throw;  // Re-throw to see the error
    }
}

void Task::fulfill_promise_exception(std::exception_ptr ex) {
    try {
        promise_->set_exception(ex);
        completed_ = true;
    } catch (const std::future_error& e) {
        // Promise already set - ignore
        DFTRACER_UTILS_LOG_WARN(
            "Task '%s' promise already fulfilled with exception",
            name_.c_str());
    }
}

std::any Task::apply_combiner(const std::vector<std::any>& inputs) const {
    if (!has_custom_combiner_) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Task has no custom combiner");
    }
    return input_combiner_(inputs);
}

bool Task::validate_connection(std::type_index from, std::type_index to) const {
    // Void can connect to anything (synchronization)
    if (from == typeid(void)) {
        return true;
    }

    // Exact type match
    return from == to;
}

}  // namespace dftracer::utils
