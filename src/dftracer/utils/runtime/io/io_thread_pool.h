#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_THREAD_POOL_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_THREAD_POOL_H

#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace dftracer::utils::io {

/// How the pool sizes itself. An eager pool starts its whole cap. Any other
/// pool starts one thread, adds one when work queues with no idle thread, and
/// retires an idle thread above one after `keepalive` without work.
struct IoPoolOptions {
    bool eager = true;
    std::chrono::milliseconds keepalive{250};
};

/// Small, dedicated thread pool for I/O backends.
/// Runs blocking syscalls off the executor's compute workers.
///
/// Supports optional batched submission: when batch_threshold > 0,
/// submit() queues work without waking threads and auto-flushes
/// when the pending count reaches the threshold.  flush() submits
/// all queued work with a single notify_all (reducing futex wakes
/// from N to 1 per batch).  When batch_threshold == 0 (default),
/// submit() wakes one thread immediately (legacy behavior).
///
/// The thread count given to the constructor is the cap.
class IoThreadPool {
   public:
    explicit IoThreadPool(std::size_t num_threads = 4,
                          unsigned batch_threshold = 0,
                          IoPoolOptions options = {});
    ~IoThreadPool();

    IoThreadPool(const IoThreadPool&) = delete;
    IoThreadPool& operator=(const IoThreadPool&) = delete;

    /// Start the pool threads.
    void start();

    /// Stop and join all threads.
    void stop();

    /// Raise the cap to `threads`. A smaller count leaves the cap as it is.
    /// An eager pool that runs starts the threads up to the cap at once, and
    /// `eager` replaces the pool's mode.
    void grow(std::size_t threads, bool eager);

    /// Threads that left after the keepalive since the pool was made. For
    /// tests.
    std::size_t retirements() const noexcept {
        return retirements_.load(std::memory_order_acquire);
    }

    /// Threads that are alive now. For tests.
    std::size_t live_threads() const noexcept {
        return live_.load(std::memory_order_acquire);
    }

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

    /// Operations that can wait on a peer indefinitely stop waiting and fail
    /// with -ECANCELED, both those queued and those blocked. Sticky until the
    /// next start().
    void cancel() noexcept {
        cancelling_.store(true, std::memory_order_release);
    }
    bool cancelling() const noexcept {
        return cancelling_.load(std::memory_order_acquire);
    }

   private:
    void worker_loop();
    void wake(std::size_t items);
    bool spawn_locked();
    void reap_locked();

    std::atomic<std::size_t> num_threads_;
    unsigned batch_threshold_;
    std::chrono::milliseconds keepalive_;
    // eager_, idle_, retired_ and threads_ change under mutex_.
    bool eager_;
    std::size_t idle_ = 0;
    dstd::Vector<std::thread::id> retired_;
    bool spawn_warned_ = false;
    std::atomic<std::size_t> live_{0};
    std::atomic<std::size_t> retirements_{0};
    // Written under mutex_; atomic so flush() can skip the lock when empty.
    std::atomic<unsigned> unflushed_count_{0};
    dstd::Vector<std::thread> threads_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> tracked_{0};
    std::atomic<bool> cancelling_{false};
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_THREAD_POOL_H
