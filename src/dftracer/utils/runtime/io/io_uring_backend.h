// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_URING_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_URING_BACKEND_H
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/runtime/io/io_uring_wrapper.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::runtime::io {

/**
 * @brief io_uring backend through raw system calls, no liburing.
 *
 * An executor worker submits positional file operations to a ring of its own,
 * where a read of cached data ends inside the submit, with no thread woken. The
 * kernel ties a request to the thread that submits it and cancels the request
 * when that thread exits, so a write from a short-lived thread would fail on a
 * healthy disk. So a worker drains its ring before it exits, and everything
 * that may never end or comes from any other thread goes to one owner thread's
 * ring: those threads push to a stack and wake the owner when it sleeps. An
 * operation with no ring opcode runs on the thread pool.
 *
 * A worker reaps its own ring. A worker that runs long or blocks leaves
 * completions behind, so idle workers and the sysmon reap them through poll().
 *
 * Features come from the kernel's answer to the ring setup, never its version.
 * A ring with no timed enter ends a timed wait with a timeout request.
 */
class IoUringBackend : public ThreadPoolFileOps {
   public:
    /// `masked_features` hides IORING_FEAT_* bits, so a test can run the path
    /// of an older kernel on a newer one.
    explicit IoUringBackend(std::size_t pool_size = 4,
                            IoPoolOptions pool_options = {},
                            unsigned masked_features = 0);
    ~IoUringBackend() override;

    /// Whether this kernel has what the backend needs.
    bool probe();

    /// Throws when the kernel refuses a ring, because no operation could end.
    void start() override;
    void stop() override;
    void submit(IoOp& op) override;
    int flush() override;
    std::size_t poll(int timeout_ms) override;
    void attach_thread() override;
    void detach_thread() override;
    void* prepare_park() override;
    void park(void* token, std::int64_t timeout_ms) override;
    void unpark(void* token) noexcept override;
    void finish_park(void* token) override;
    dstd::String name() const override { return "io_uring"; }

    std::size_t in_flight() const override;

    /// Cancels the reads, accepts and sends on the ring, on every kernel, and
    /// fails those submitted afterwards with -ECANCELED.
    void cancel_pending() override;

   private:
    struct WorkerRing;

    WorkerRing* calling_worker_ring(IoOpKind kind) noexcept;
    void fill_worker(WorkerRing& worker) noexcept;
    void flush_worker(WorkerRing& worker) noexcept;
    void quiet_worker(WorkerRing& worker) noexcept;
    static std::size_t drain_worker(WorkerRing& worker) noexcept;
    static std::size_t reap_worker(WorkerRing& worker) noexcept;

    void ring_loop() noexcept;
    void wake() noexcept;
    void take_incoming() noexcept;
    void fill_ring() noexcept;
    std::size_t reap() noexcept;
    void fail(IoOp& op, ssize_t result) noexcept;
    void unlink_waiting(IoOp& op) noexcept;
    void cancel_waiting() noexcept;

    const unsigned masked_features_;
    // Set in start(), before any worker parks.
    bool timed_enter_ = false;

    // Only the ring thread touches these.
    uring::Ring ring_;
    IoOp* backlog_head_ = nullptr;
    IoOp* backlog_tail_ = nullptr;
    IoOp* waiting_head_ = nullptr;
    unsigned in_ring_ = 0;
    bool wake_armed_ = false;
    std::uint64_t wake_value_ = 0;

    dstd::Thread thread_;
    dstd::ScopedFd wake_fd_;
    std::atomic<IoOp*> incoming_{nullptr};
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<bool> parked_{false};
    std::atomic<bool> cancelling_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> stopping_{false};

    // Grows only, until stop(). A retired worker's ring is reused, so thieves
    // never read freed memory.
    std::mutex worker_rings_mutex_;
    dstd::UniquePtr<WorkerRing> worker_rings_owned_;
    std::atomic<WorkerRing*> worker_rings_{nullptr};
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_HAVE_IO_URING
#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_URING_BACKEND_H
