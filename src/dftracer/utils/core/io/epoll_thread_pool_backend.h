#ifndef DFTRACER_UTILS_CORE_IO_EPOLL_THREAD_POOL_BACKEND_H
#define DFTRACER_UTILS_CORE_IO_EPOLL_THREAD_POOL_BACKEND_H
#ifdef __linux__

#include <dftracer/utils/core/io/fd_handle.h>
#include <dftracer/utils/core/io/io_completion_thread.h>
#include <dftracer/utils/core/io/thread_pool_file_ops.h>

#include <cstddef>
#include <string>

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// epoll + thread pool I/O backend.
/// File I/O is handled by the thread pool via ThreadPoolFileOps (epoll cannot
/// watch regular files). An epoll reactor is set up for future network I/O
/// (socket read/write/accept). The completion thread blocks on epoll_wait,
/// currently only watching an eventfd used for clean shutdown.
class EpollThreadPoolBackend : public ThreadPoolFileOps {
   public:
    explicit EpollThreadPoolBackend(Executor& executor,
                                    std::size_t pool_size = 4,
                                    unsigned batch_threshold = 0);
    ~EpollThreadPoolBackend() override;

    void start() override;
    void stop() override;
    std::string name() const override { return "epoll+threadpool"; }

   private:
    /// Epoll loop run by the completion thread. Currently only watches
    /// the eventfd for shutdown; will be extended for socket I/O.
    void epoll_loop();

    IoCompletionThread completion_thread_;
    FdHandle epoll_fd_;
    FdHandle event_fd_;
};

}  // namespace dftracer::utils::io

#endif  // __linux__
#endif  // DFTRACER_UTILS_CORE_IO_EPOLL_THREAD_POOL_BACKEND_H
