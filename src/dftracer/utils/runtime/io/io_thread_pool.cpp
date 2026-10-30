#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/runtime/io/io_thread_pool.h>

#include <chrono>

namespace dftracer::utils::io {

namespace {
// How long an I/O thread waits for work before it gives back its idle pages.
constexpr std::chrono::milliseconds IDLE_TRIM_AFTER{50};
}  // namespace

IoThreadPool::IoThreadPool(std::size_t num_threads, unsigned batch_threshold)
    : num_threads_(num_threads == 0 ? 4 : num_threads),
      batch_threshold_(batch_threshold) {}

IoThreadPool::~IoThreadPool() { stop(); }

void IoThreadPool::start() {
    if (running_.load()) return;
    cancelling_.store(false, std::memory_order_release);
    running_ = true;
    const std::size_t count = num_threads_.load(std::memory_order_acquire);
    threads_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads_.emplace_back(&IoThreadPool::worker_loop, this);
    }
}

void IoThreadPool::grow(std::size_t threads) {
    // Under the lock stop() takes to clear running_, so a pool that stops
    // either joins the new threads or never gets them.
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = num_threads_.load(std::memory_order_acquire);
    if (threads <= count) return;
    if (running_.load()) {
        for (std::size_t i = count; i < threads; ++i)
            threads_.emplace_back(&IoThreadPool::worker_loop, this);
    }
    num_threads_.store(threads, std::memory_order_release);
}

void IoThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) return;
        running_ = false;
    }
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
            const unsigned unflushed =
                unflushed_count_.load(std::memory_order_relaxed) + 1;
            if (unflushed >= batch_threshold_) {
                unflushed_count_.store(0, std::memory_order_relaxed);
                should_notify = true;
            } else {
                unflushed_count_.store(unflushed, std::memory_order_relaxed);
            }
        }
    }
    if (should_notify) {
        if (batch_threshold_ == 0) {
            cv_.notify_one();  // Legacy: wake one worker per op
        } else {
            wake(batch_threshold_);
        }
    }
}

std::size_t IoThreadPool::flush() {
    if (unflushed_count_.load(std::memory_order_relaxed) == 0) return 0;
    std::size_t count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = unflushed_count_.load(std::memory_order_relaxed);
        unflushed_count_.store(0, std::memory_order_relaxed);
    }
    wake(count);
    return count;
}

void IoThreadPool::wake(std::size_t items) {
    // A woken worker drains the queue, so waking the whole pool per flush just
    // thunders it; wake only as many workers as pending items.
    const std::size_t pool = num_threads_.load(std::memory_order_relaxed);
    const std::size_t n = items < pool ? items : pool;
    for (std::size_t i = 0; i < n; ++i) {
        cv_.notify_one();
    }
}

void IoThreadPool::worker_loop() {
    while (running_.load()) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] {
                return !running_.load() || !queue_.empty();
            };
            if (!cv_.wait_for(lock, IDLE_TRIM_AFTER, ready)) {
                // Only this thread may purge its heap.
                lock.unlock();
                memory::trim_thread();
                lock.lock();
                cv_.wait(lock, ready);
            }
            if (!running_.load() && queue_.empty()) return;
            if (queue_.empty()) continue;
            fn = std::move(queue_.front());
            queue_.pop();
        }
        fn();
    }
}

}  // namespace dftracer::utils::io
