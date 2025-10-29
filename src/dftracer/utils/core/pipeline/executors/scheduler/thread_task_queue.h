#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_THREAD_TASK_QUEUE_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_THREAD_TASK_QUEUE_H

#include <dftracer/utils/core/common/typedefs.h>

#include <any>
#include <deque>
#include <functional>
#include <mutex>

namespace dftracer::utils {

// Forward declaration for Task
class Task;

// TaskItem represents a task to be executed with its input
struct TaskItem {
    TaskIndex task_id;
    Task* task_ptr;  // Direct access to Task for execution
    std::any input;
    std::function<void(std::any)> completion_callback;

    // Default constructor
    TaskItem() : task_id(-1), task_ptr(nullptr) {}

    // Constructor
    TaskItem(TaskIndex id, Task* ptr, std::any inp,
             std::function<void(std::any)> callback)
        : task_id(id),
          task_ptr(ptr),
          input(std::move(inp)),
          completion_callback(std::move(callback)) {}

    // Move constructor
    TaskItem(TaskItem&& other) noexcept
        : task_id(other.task_id),
          task_ptr(other.task_ptr),
          input(std::move(other.input)),
          completion_callback(std::move(other.completion_callback)) {
        other.task_id = -1;
        other.task_ptr = nullptr;
    }

    // Move assignment operator
    TaskItem& operator=(TaskItem&& other) noexcept {
        if (this != &other) {
            task_id = other.task_id;
            task_ptr = other.task_ptr;
            input = std::move(other.input);
            completion_callback = std::move(other.completion_callback);
            other.task_id = -1;
            other.task_ptr = nullptr;
        }
        return *this;
    }

    // Delete copy constructor and assignment to prevent accidental copying
    TaskItem(const TaskItem&) = delete;
    TaskItem& operator=(const TaskItem&) = delete;
};

// TaskQueue is a thread-safe queue of tasks with work-stealing support
class TaskQueue {
   private:
    std::deque<TaskItem> queue_;
    std::mutex mutex_;

   public:
    // Add a task to the queue (for the owner thread)
    void push(TaskItem item);

    // Remove a task from the front of the queue (for the owner thread)
    bool pop(TaskItem& item);

    // Remove a task from the back of the queue (for work stealing)
    bool steal(TaskItem& item);

    // Get the current size of the queue
    size_t size();

    // Check if the queue is empty
    bool empty();
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_THREAD_TASK_QUEUE_H
