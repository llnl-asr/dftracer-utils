#ifndef DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GROUP_H
#define DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GROUP_H

#include <dftracer/utils/core/common/error.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

namespace dftracer::utils {
// Forward declaration
class Task;
}  // namespace dftracer::utils

namespace dftracer::utils::task_graph {

/**
 * TaskGroup<T> - Handle to a collection of tasks that produce type T
 *
 * Returned by TaskGraph methods to provide access to underlying tasks.
 * Used for:
 * - Chaining operations (transform, reduce, fan_out, fan_in)
 * - Accessing individual tasks for external dependencies
 * - Getting terminal tasks for output
 *
 * The type parameter T represents the output type of tasks in this group.
 * It's used for compile-time type checking when chaining operations.
 */
template <typename T>
class TaskGroup {
   public:
    /**
     * Default constructor - creates empty group
     */
    TaskGroup() = default;

    /**
     * Construct from vector of tasks
     */
    explicit TaskGroup(std::vector<std::shared_ptr<Task>> tasks)
        : tasks_(std::move(tasks)) {}

    /**
     * Construct from single task
     */
    explicit TaskGroup(std::shared_ptr<Task> task) : tasks_{std::move(task)} {}

    /**
     * Get all tasks in this group
     */
    const std::vector<std::shared_ptr<Task>>& tasks() const { return tasks_; }

    /**
     * Get single task (asserts size == 1)
     *
     * Use this when you know the group contains exactly one task,
     * e.g., after reduce() which produces a single output.
     */
    std::shared_ptr<Task> task() const {
        if (tasks_.size() != 1) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "TaskGroup::task() called on group with " +
                                        std::to_string(tasks_.size()) +
                                        " tasks (expected 1)");
        }
        return tasks_[0];
    }

    /**
     * Get task at index
     */
    std::shared_ptr<Task> at(std::size_t index) const {
        if (index >= tasks_.size()) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "TaskGroup index out of range");
        }
        return tasks_[index];
    }

    /**
     * Get task at index (no bounds checking)
     */
    std::shared_ptr<Task> operator[](std::size_t index) const {
        return tasks_[index];
    }

    /**
     * Number of tasks in group
     */
    std::size_t size() const { return tasks_.size(); }

    /**
     * Check if group is empty
     */
    bool empty() const { return tasks_.empty(); }

    /**
     * Add a task to the group
     */
    void add(std::shared_ptr<Task> task) { tasks_.push_back(std::move(task)); }

    /**
     * Reserve capacity
     */
    void reserve(std::size_t capacity) { tasks_.reserve(capacity); }

    // Iterator support
    auto begin() { return tasks_.begin(); }
    auto end() { return tasks_.end(); }
    auto begin() const { return tasks_.begin(); }
    auto end() const { return tasks_.end(); }
    auto cbegin() const { return tasks_.cbegin(); }
    auto cend() const { return tasks_.cend(); }

   private:
    std::vector<std::shared_ptr<Task>> tasks_;
};

/**
 * Specialization for void - group of tasks with no output
 */
template <>
class TaskGroup<void> {
   public:
    TaskGroup() = default;
    explicit TaskGroup(std::vector<std::shared_ptr<Task>> tasks)
        : tasks_(std::move(tasks)) {}
    explicit TaskGroup(std::shared_ptr<Task> task) : tasks_{std::move(task)} {}

    const std::vector<std::shared_ptr<Task>>& tasks() const { return tasks_; }

    std::shared_ptr<Task> task() const {
        if (tasks_.size() != 1) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "TaskGroup::task() called on group with " +
                                        std::to_string(tasks_.size()) +
                                        " tasks (expected 1)");
        }
        return tasks_[0];
    }

    std::shared_ptr<Task> at(std::size_t index) const {
        if (index >= tasks_.size()) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "TaskGroup index out of range");
        }
        return tasks_[index];
    }

    std::shared_ptr<Task> operator[](std::size_t index) const {
        return tasks_[index];
    }

    std::size_t size() const { return tasks_.size(); }
    bool empty() const { return tasks_.empty(); }
    void add(std::shared_ptr<Task> task) { tasks_.push_back(std::move(task)); }
    void reserve(std::size_t capacity) { tasks_.reserve(capacity); }

    auto begin() { return tasks_.begin(); }
    auto end() { return tasks_.end(); }
    auto begin() const { return tasks_.begin(); }
    auto end() const { return tasks_.end(); }
    auto cbegin() const { return tasks_.cbegin(); }
    auto cend() const { return tasks_.cend(); }

   private:
    std::vector<std::shared_ptr<Task>> tasks_;
};

}  // namespace dftracer::utils::task_graph

#endif  // DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_GROUP_H
