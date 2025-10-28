#include <dftracer/utils/core/pipeline/task_queue.h>

namespace dftracer::utils {

void TaskQueue::push(TaskItem item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
    }
    cv_.notify_one();
}

std::optional<TaskItem> TaskQueue::pop(bool blocking) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (blocking) {
        // Wait until queue has items or shutdown
        cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
    }

    // Check if we should return
    if (queue_.empty()) {
        return std::nullopt;
    }

    TaskItem item = std::move(queue_.front());
    queue_.pop();
    return item;
}

void TaskQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
}

bool TaskQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace dftracer::utils
