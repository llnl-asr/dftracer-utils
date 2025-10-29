#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/function_task.h>

namespace dftracer::utils {

TaskIndex Pipeline::add_task(std::unique_ptr<Task> task, TaskIndex depends_on) {
    TaskIndex index = static_cast<TaskIndex>(nodes_.size());
    nodes_.push_back(std::move(task));
    dependencies_.push_back({});
    dependents_.push_back({});
    if (depends_on >= 0) {
        add_dependency(depends_on, index);
    }
    return index;
}

void Pipeline::add_dependency(TaskIndex from, TaskIndex to) {
    if (from >= static_cast<TaskIndex>(nodes_.size()) ||
        to >= static_cast<TaskIndex>(nodes_.size())) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Invalid task index");
    }

    // For edge: from -> to
    dependencies_[to].push_back(from);  // "to" depends on "from"
    dependents_[from].push_back(to);    // "from" has dependent "to"
}

bool Pipeline::validate_types() const {
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(nodes_.size()); ++i) {
        for (TaskIndex dependent : dependencies_[i]) {
            if (nodes_[dependent]->get_output_type() !=
                nodes_[i]->get_input_type()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Type mismatch between task %d (output: %s) and task %d "
                    "(expected input: %s)",
                    dependent, nodes_[dependent]->get_output_type().name(), i,
                    nodes_[i]->get_input_type().name());
                return false;
            }
        }
    }
    return true;
}

bool Pipeline::has_cycles() const {
    std::vector<int> in_degree(nodes_.size(), 0);
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(nodes_.size()); ++i) {
        in_degree[i] = static_cast<int>(
            dependencies_[i].size());  // Count static dependencies
    }

    std::queue<TaskIndex> queue;
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(nodes_.size()); ++i) {
        if (in_degree[i] == 0) {
            queue.push(i);
        }
    }

    std::size_t processed = 0;
    while (!queue.empty()) {
        TaskIndex current = queue.front();
        queue.pop();
        processed++;

        for (TaskIndex dependent : dependents_[current]) {
            in_degree[dependent]--;
            if (in_degree[dependent] == 0) {
                queue.push(dependent);
            }
        }
    }

    return processed != nodes_.size();
}

std::vector<TaskIndex> Pipeline::topological_sort() const {
    std::vector<TaskIndex> result;
    std::vector<std::size_t> in_degree(nodes_.size(), 0);

    for (TaskIndex i = 0; i < static_cast<TaskIndex>(nodes_.size()); ++i) {
        in_degree[i] = dependencies_[i].size();  // Count static dependencies
    }

    std::queue<TaskIndex> queue;
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(nodes_.size()); ++i) {
        if (in_degree[i] == 0) {
            queue.push(i);
        }
    }

    while (!queue.empty()) {
        TaskIndex current = queue.front();
        queue.pop();
        result.push_back(current);

        for (TaskIndex dependent : dependents_[current]) {
            in_degree[dependent]--;
            if (in_degree[dependent] == 0) {
                queue.push(dependent);
            }
        }
    }

    return result;
}

void Pipeline::chain(Pipeline&& other) {
    if (other.empty()) {
        return;
    }

    TaskIndex offset = static_cast<TaskIndex>(nodes_.size());

    for (auto& task : other.nodes_) {
        nodes_.push_back(std::move(task));
        dependencies_.push_back({});
        dependents_.push_back({});
    }

    for (std::size_t i = 0; i < other.dependencies_.size(); ++i) {
        TaskIndex new_index = offset + static_cast<TaskIndex>(i);
        for (TaskIndex dep : other.dependencies_[i]) {
            TaskIndex new_dep = offset + dep;
            dependencies_[new_index].push_back(new_dep);
            dependents_[new_dep].push_back(new_index);
        }
    }
}

}  // namespace dftracer::utils
