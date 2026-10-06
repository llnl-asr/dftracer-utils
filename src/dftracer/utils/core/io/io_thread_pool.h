#ifndef DFTRACER_UTILS_CORE_IO_IO_THREAD_POOL_H
#define DFTRACER_UTILS_CORE_IO_IO_THREAD_POOL_H

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

    /// File operations submitted whose completion has not been delivered. A
    /// completion is delivered (the waiting coroutine enqueued) before
    /// end_tracked(), so a zero count means no resumption is still coming.
    void begin_tracked() noexcept {
        tracked_.fetch_add(1, std::memory_order_acq_rel);
    }
    void end_tracked() noexcept {
        tracked_.fetch_sub(1, std::memory_order_acq_rel);
    }
    std::size_t tracked() const noexcept {
        return tracked_.load(std::memory_order_acquire);
    }

   private:
    void worker_loop();
    void wake(std::size_t items);

    std::size_t num_threads_;
    unsigned batch_threshold_;
    unsigned unflushed_count_ = 0;  // protected by mutex_
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> tracked_{0};
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_IO_THREAD_POOL_H
