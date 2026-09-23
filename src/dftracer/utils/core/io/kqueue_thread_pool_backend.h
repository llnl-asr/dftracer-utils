#ifndef DFTRACER_UTILS_CORE_IO_KQUEUE_THREAD_POOL_BACKEND_H
#define DFTRACER_UTILS_CORE_IO_KQUEUE_THREAD_POOL_BACKEND_H
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define DFTRACER_UTILS_HAVE_KQUEUE 1

#include <dftracer/utils/core/io/fd_handle.h>
#include <dftracer/utils/core/io/io_completion_thread.h>
#include <dftracer/utils/core/io/thread_pool_file_ops.h>

#include <cstddef>
#include <string>

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// kqueue + thread pool I/O backend (macOS, FreeBSD, etc.).
/// File I/O is handled by the thread pool via ThreadPoolFileOps (kqueue cannot
/// watch regular files). A kqueue reactor is set up for future network I/O
/// (socket read/write/accept). The completion thread blocks on kevent(),
/// currently only watching a user event used for clean shutdown.
class KqueueThreadPoolBackend : public ThreadPoolFileOps {
   public:
    explicit KqueueThreadPoolBackend(Executor& executor,
                                     std::size_t pool_size = 4,
                                     unsigned batch_threshold = 0);
    ~KqueueThreadPoolBackend() override;

    void start() override;
    void stop() override;
    std::string name() const override { return "kqueue+threadpool"; }

   private:
    /// Kqueue loop run by the completion thread. Currently only watches
    /// a user event for shutdown; will be extended for socket I/O.
    void kqueue_loop();

    IoCompletionThread completion_thread_;
    FdHandle kqueue_fd_;

    /// User event identifier for shutdown signaling.
    static constexpr uintptr_t SHUTDOWN_IDENT = 0xDEAD;
};

}  // namespace dftracer::utils::io

#endif  // kqueue platforms
#endif  // DFTRACER_UTILS_CORE_IO_KQUEUE_THREAD_POOL_BACKEND_H
