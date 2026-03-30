#include <dftracer/utils/core/io/io_thread_pool.h>

namespace dftracer::utils::io {

IoThreadPool::IoThreadPool(std::size_t num_threads, unsigned batch_threshold)
    : num_threads_(num_threads == 0 ? 4 : num_threads),
      batch_threshold_(batch_threshold) {}

IoThreadPool::~IoThreadPool() { stop(); }

void IoThreadPool::start() {
    if (running_.load()) return;
    running_ = true;
    threads_.reserve(num_threads_);
    for (std::size_t i = 0; i < num_threads_; ++i) {
        threads_.emplace_back(&IoThreadPool::worker_loop, this);
    }
}

void IoThreadPool::stop() {
    if (!running_.load()) return;
    running_ = false;
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void IoThreadPool::submit(std::function<void()> fn) {
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(fn));
        if (batch_threshold_ == 0) {
            // Immediate mode (legacy behavior).
            should_notify = true;
        } else {
            ++unflushed_count_;
            if (unflushed_count_ >= batch_threshold_) {
                unflushed_count_ = 0;
                should_notify = true;
            }
        }
    }
    if (should_notify) {
        if (batch_threshold_ == 0) {
            cv_.notify_one();  // Legacy: wake one worker per op
        } else {
            cv_.notify_all();  // Batch: wake all to drain queue
        }
    }
}

std::size_t IoThreadPool::flush() {
    std::size_t count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = unflushed_count_;
        unflushed_count_ = 0;
    }
    if (count > 0) {
        cv_.notify_all();
    }
    return count;
}

void IoThreadPool::worker_loop() {
    while (running_.load()) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,
                     [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load() && queue_.empty()) return;
            if (queue_.empty()) continue;
            fn = std::move(queue_.front());
            queue_.pop();
        }
        fn();
    }
}

}  // namespace dftracer::utils::io
