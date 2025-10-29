#include <dftracer/utils/core/pipeline/executors/scheduler/thread_task_queue.h>

namespace dftracer::utils {

void TaskQueue::push(TaskItem item) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(item));
}

bool TaskQueue::pop(TaskItem& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }

    // Owner thread takes from the front (FIFO for owner)
    item = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool TaskQueue::steal(TaskItem& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }

    // Stealing threads take from the back (to reduce contention)
    item = std::move(queue_.back());
    queue_.pop_back();
    return true;
}

size_t TaskQueue::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool TaskQueue::empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

}  // namespace dftracer::utils
