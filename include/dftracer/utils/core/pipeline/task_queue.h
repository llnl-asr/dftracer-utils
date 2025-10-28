#ifndef DFTRACER_UTILS_CORE_PIPELINE_TASK_QUEUE_H
#define DFTRACER_UTILS_CORE_PIPELINE_TASK_QUEUE_H

#include <any>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>

namespace dftracer::utils {

class Task;

/**
 * TaskItem - Item in the task queue
 */
struct TaskItem {
    std::shared_ptr<Task> task;
    std::shared_ptr<std::any> input;  // Shared to avoid copies

    TaskItem() = default;
    TaskItem(std::shared_ptr<Task> t, std::shared_ptr<std::any> i)
        : task(std::move(t)), input(std::move(i)) {}
};

/**
 * TaskQueue - Thread-safe queue for scheduler-executor communication
 *
 * Features:
 * - Thread-safe push/pop operations
 * - Blocking and non-blocking pop
 * - Graceful shutdown support
 */
class TaskQueue {
   private:
    std::queue<TaskItem> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

   public:
    TaskQueue() = default;
    ~TaskQueue() { shutdown(); }

    // Prevent copying
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // Allow moving
    TaskQueue(TaskQueue&&) = default;
    TaskQueue& operator=(TaskQueue&&) = default;

    /**
     * Push an item to the queue
     */
    void push(TaskItem item);

    /**
     * Pop an item from the queue
     * @param blocking If true, blocks until an item is available or shutdown
     * @return std::nullopt if queue is shutdown and empty
     */
    std::optional<TaskItem> pop(bool blocking = true);

    /**
     * Shutdown the queue (signals all waiting threads)
     */
    void shutdown();

    /**
     * Check if queue is empty
     */
    bool empty() const;

    /**
     * Get queue size
     */
    size_t size() const;

    /**
     * Check if queue is running
     */
    bool is_running() const { return running_.load(); }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_TASK_QUEUE_H
