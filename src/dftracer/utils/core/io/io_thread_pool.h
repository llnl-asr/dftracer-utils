#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace dftracer::utils::io {

/// Small, dedicated thread pool for I/O backends.
/// Runs blocking syscalls off the executor's compute workers.
///
/// Supports optional batched submission: when batch_threshold > 0,
/// submit() queues work without waking threads and auto-flushes
/// when the pending count reaches the threshold.  flush() submits
/// all queued work with a single notify_all (reducing futex wakes
/// from N to 1 per batch).  When batch_threshold == 0 (default),
/// submit() wakes one thread immediately (legacy behavior).
class IoThreadPool {
   public:
    explicit IoThreadPool(std::size_t num_threads = 4,
                          unsigned batch_threshold = 0);
    ~IoThreadPool();

    IoThreadPool(const IoThreadPool&) = delete;
    IoThreadPool& operator=(const IoThreadPool&) = delete;

    /// Start the pool threads.
    void start();

    /// Stop and join all threads.
    void stop();

    /// Submit work. When batch_threshold > 0, queues without waking
    /// and auto-flushes at threshold.  Thread-safe.
    void submit(std::function<void()> fn);

    /// Flush all queued work to pool threads (single notify_all).
    /// Returns number of unflushed items that were pending.
    std::size_t flush();

   private:
    void worker_loop();

    std::size_t num_threads_;
    unsigned batch_threshold_;
    unsigned unflushed_count_ = 0;  // protected by mutex_
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

}  // namespace dftracer::utils::io
