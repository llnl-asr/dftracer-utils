// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_URING_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_URING_BACKEND_H
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/runtime/io/io_uring_wrapper.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::runtime::io {

/**
 * @brief io_uring backend through raw system calls, no liburing.
 *
 * One thread owns the ring and is the only one that enters the kernel. The
 * kernel ties a request to the thread that submits it and cancels the request
 * when that thread exits, so a write from a short-lived thread would fail on a
 * healthy disk. Other threads push to a stack and wake the owner when it
 * sleeps. An operation with no ring opcode runs on the thread pool.
 */
class IoUringBackend : public ThreadPoolFileOps {
   public:
    explicit IoUringBackend(std::size_t pool_size = 4,
                            IoPoolOptions pool_options = {});
    ~IoUringBackend() override;

    /// Whether this kernel has what the backend needs.
    bool probe();

    /// Throws when the kernel refuses a ring, because no operation could end.
    void start() override;
    void stop() override;
    void submit(IoOp& op) override;
    int flush() override;
    dstd::String name() const override { return "io_uring"; }

    std::size_t in_flight() const override {
        return ThreadPoolFileOps::in_flight() +
               in_flight_.load(std::memory_order_acquire);
    }

    /// Cancels the reads, accepts and sends on the ring, on every kernel, and
    /// fails those submitted afterwards with -ECANCELED.
    void cancel_pending() override;

   private:
    void ring_loop() noexcept;
    void wake() noexcept;
    void take_incoming() noexcept;
    void fill_ring() noexcept;
    std::size_t reap() noexcept;
    void fail(IoOp& op, ssize_t result) noexcept;
    void unlink_waiting(IoOp& op) noexcept;
    void cancel_waiting() noexcept;

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
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_HAVE_IO_URING
#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_URING_BACKEND_H
