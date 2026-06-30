#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>

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

    // Register as reader for smart value release
    parent->result_.add_reader();

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

coro::CoroTask<std::any> Task::execute(CoroScope& context,
                                       const std::any& input) {
    try {
        std::any result = co_await func_(context, input);
        co_return result;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Task '%s' execution failed: %s", get_name(),
                                 e.what());
        throw;
    }
}

void Task::set_result(std::any result) { result_.set_value(std::move(result)); }

void Task::set_exception(std::exception_ptr ex) { result_.set_exception(ex); }

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

    // std::any can accept any type (for tap, logging, etc.)
    if (to == typeid(std::any)) {
        return true;
    }

    // Exact type match
    return from == to;
}

std::shared_ptr<Task> Task::operator&(std::shared_ptr<Task> other) {
    // Create a combiner task that depends on both this and other
    // When a task has multiple parents, it receives a vector<any> as input
    auto combiner = make_task(
        [](CoroScope&,
           const std::vector<std::any>& inputs) -> coro::CoroTask<std::any> {
            // inputs[0] is from first parent (this), inputs[1] is from second
            // (other)
            if (inputs.size() != 2) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    "AND combiner expects exactly 2 inputs");
            }
            // Return tuple of both results
            co_return std::make_any<std::tuple<std::any, std::any>>(inputs[0],
                                                                    inputs[1]);
        },
        "AND_combiner");

    // Combiner depends on both tasks
    combiner->depends_on(shared_from_this());
    combiner->depends_on(other);

    // Set a custom combiner to pass inputs as vector
    combiner->with_combiner(
        [](const std::vector<std::any>& inputs) -> std::any {
            return std::make_any<std::vector<std::any>>(inputs);
        });

    return combiner;
}

std::shared_ptr<Task> Task::operator^(std::shared_ptr<Task> tap_task) {
    if (!tap_task) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Tap task cannot be null");
    }
    auto passthrough = make_task(
        [](CoroScope&, const std::any& input) -> coro::CoroTask<std::any> {
            co_return input;
        },
        "TAP_passthrough");
    passthrough->depends_on(shared_from_this());
    tap_task->depends_on(shared_from_this());
    return passthrough;
}

}  // namespace dftracer::utils
