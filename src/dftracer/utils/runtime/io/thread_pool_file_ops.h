// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_THREAD_POOL_FILE_OPS_H
#define DFTRACER_UTILS_RUNTIME_IO_THREAD_POOL_FILE_OPS_H

#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_thread_pool.h>

#include <cstddef>

namespace dftracer::utils::runtime::io {

/// Runs the system call of @p op on the calling thread. Gives its value or a
/// negative errno.
ssize_t run_blocking(const IoOp& op) noexcept;

/// Runs each operation as a blocking call on a thread pool. The thread pool
/// backends add their reactor bits to it (start, stop, name, event loop).
class ThreadPoolFileOps : public IoBackend {
   public:
    explicit ThreadPoolFileOps(std::size_t pool_size = 4,
                               unsigned batch_threshold = 0,
                               IoPoolOptions pool_options = {});

    void submit(IoOp& op) override;

    /// Thread-pool backends complete via callbacks; nothing to reap.
    std::size_t poll(int timeout_ms) override;

    /// Flush pending batched submissions.
    int flush() override;

    std::size_t in_flight() const override { return pool_.tracked(); }

    bool grow_threads(std::size_t threads, bool eager) override {
        pool_.grow(threads, eager);
        return true;
    }

    /// Reads, accepts and sends that wait on a peer complete with -ECANCELED;
    /// operations that finish by themselves run to the end.
    void cancel_pending() override { pool_.cancel(); }

   protected:
    IoThreadPool pool_;

   private:
    void execute(IoOp& op) noexcept;
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_THREAD_POOL_FILE_OPS_H
