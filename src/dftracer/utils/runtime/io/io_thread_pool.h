// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_THREAD_POOL_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_THREAD_POOL_H

#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/queue.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

namespace dftracer::utils::runtime::io {

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
/// submit() queues work and wakes threads once `batch_threshold` items wait,
/// one thread per waiting item. flush() wakes threads for the items still
/// waiting. A threshold of 0 or 1 wakes a thread on every submit.
///
/// The thread count given to the constructor is the cap.
class IoThreadPool {
   public:
    explicit IoThreadPool(std::size_t thread_count = 4,
                          unsigned batch_threshold = 0,
                          IoPoolOptions options = {});
    ~IoThreadPool();

    IoThreadPool(const IoThreadPool&) = delete;
    IoThreadPool& operator=(const IoThreadPool&) = delete;

    /// Start the pool threads. Fails when no thread can start.
    dstd::Result<void> start();

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

    /// Thread-safe.
    void submit(dstd::Function<void()> fn);

    /// Returns the number of items that were waiting for a wake.
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
    /// True when a thread started, false when none could but the pool has one,
    /// an error when it has none.
    dstd::Result<bool> spawn_locked();
    void reap_locked();

    std::atomic<std::size_t> thread_count_;
    unsigned batch_threshold_;
    std::chrono::milliseconds keepalive_;
    // eager_, idle_, retired_ and threads_ change under mutex_.
    bool eager_;
    std::size_t idle_ = 0;
    dstd::Vector<dstd::Thread::Id> retired_;
    bool spawn_warned_ = false;
    std::atomic<std::size_t> live_{0};
    std::atomic<std::size_t> retirements_{0};
    // Written under mutex_; atomic so flush() can skip the lock when empty.
    std::atomic<unsigned> unflushed_count_{0};
    dstd::Vector<dstd::Thread> threads_;
    dstd::Queue<dstd::Function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> tracked_{0};
    std::atomic<bool> cancelling_{false};
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_THREAD_POOL_H
