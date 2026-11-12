// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_COMPLETION_THREAD_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_COMPLETION_THREAD_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/thread.h>

#include <atomic>

namespace dftracer::utils::runtime::io {

/// Reusable completion thread that repeatedly calls a user-provided
/// poll function. The poll function should block until work is
/// available or until woken via stop(). Used by io_uring and epoll
/// backends for their completion loops.
class IoCompletionThread {
   public:
    IoCompletionThread() = default;
    ~IoCompletionThread();

    IoCompletionThread(const IoCompletionThread&) = delete;
    IoCompletionThread& operator=(const IoCompletionThread&) = delete;

    /// Start the completion thread. The poll_fn is called in a loop
    /// until stop() is called. The poll_fn should block efficiently
    /// (e.g. io_uring_wait_cqe, epoll_wait) and check running() to
    /// know when to exit.
    void start(dstd::Function<void()> poll_fn);

    /// Signal the thread to stop (set running flag to false).
    /// The thread won't be joined until join() or stop() is called.
    void signal_stop();

    /// Signal stop and join the thread.
    void stop();

    /// Join the thread (must call signal_stop() first).
    void join();

    /// Check if the thread should keep running. Poll functions should
    /// check this to know when to exit their loop.
    bool running() const { return running_.load(std::memory_order_relaxed); }

   private:
    dstd::Thread thread_;
    std::atomic<bool> running_{false};
    dstd::Function<void()> poll_fn_;
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_COMPLETION_THREAD_H
