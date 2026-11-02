// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_KQUEUE_THREAD_POOL_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_KQUEUE_THREAD_POOL_BACKEND_H
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define DFTRACER_UTILS_HAVE_KQUEUE 1

#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/io/io_completion_thread.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>

#include <cstddef>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::io {

/// kqueue + thread pool I/O backend (macOS, FreeBSD, etc.).
/// File I/O is handled by the thread pool via ThreadPoolFileOps (kqueue cannot
/// watch regular files). A kqueue reactor is set up for future network I/O
/// (socket read/write/accept). The completion thread blocks on kevent(),
/// currently only watching a user event used for clean shutdown.
class KqueueThreadPoolBackend : public ThreadPoolFileOps {
   public:
    explicit KqueueThreadPoolBackend(coro::Executor& executor,
                                     std::size_t pool_size = 4,
                                     unsigned batch_threshold = 0,
                                     IoPoolOptions pool_options = {});
    ~KqueueThreadPoolBackend() override;

    void start() override;
    void stop() override;
    dstd::String name() const override { return "kqueue+threadpool"; }

   private:
    /// Kqueue loop run by the completion thread. Currently only watches
    /// a user event for shutdown; will be extended for socket I/O.
    void kqueue_loop();

    IoCompletionThread completion_thread_;
    dstd::ScopedFd kqueue_fd_;

    /// User event identifier for shutdown signaling.
    static constexpr uintptr_t SHUTDOWN_IDENT = 0xDEAD;
};

}  // namespace dftracer::utils::io

#endif  // kqueue platforms
#endif  // DFTRACER_UTILS_RUNTIME_IO_KQUEUE_THREAD_POOL_BACKEND_H
