#pragma once
#ifdef __linux__

#include <dftracer/utils/core/io/io_backend.h>
#include <sys/stat.h>

#include <cstddef>
#include <string>

#include "io_completion_thread.h"
#include "io_thread_pool.h"

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

struct IoRequest;  // defined in thread_pool_backend.h

/// epoll + thread pool I/O backend.
/// File I/O is handled by the thread pool (epoll cannot watch regular
/// files). An epoll reactor is set up for future network I/O (socket
/// read/write/accept). The completion thread blocks on epoll_wait,
/// currently only watching an eventfd used for clean shutdown.
class EpollThreadPoolBackend : public IoBackend {
   public:
    explicit EpollThreadPoolBackend(Executor& executor,
                                    std::size_t pool_size = 4,
                                    unsigned batch_threshold = 0);
    ~EpollThreadPoolBackend() override;

    void start() override;
    void stop() override;

    IoAwaitable submit_read(int fd, void* buf, std::size_t len) override;
    IoAwaitable submit_write(int fd, const void* buf, std::size_t len) override;
    IoAwaitable submit_pread(int fd, void* buf, std::size_t len,
                             off_t offset) override;
    IoAwaitable submit_pwrite(int fd, const void* buf, std::size_t len,
                              off_t offset) override;
    IoAwaitable submit_open(const char* path, int flags, mode_t mode) override;
    IoAwaitable submit_close(int fd) override;
    IoAwaitable submit_fsync(int fd) override;
    IoAwaitable submit_ftruncate(int fd, off_t length) override;
    IoAwaitable submit_fstat(int fd, struct stat* buf) override;
    IoAwaitable submit_accept(int listen_fd, struct sockaddr* addr,
                              socklen_t* addrlen) override;
    IoAwaitable submit_recv(int fd, void* buf, std::size_t len,
                            int flags) override;
    IoAwaitable submit_send(int fd, const void* buf, std::size_t len,
                            int flags) override;
    IoAwaitable submit_readv(int fd, const struct iovec* iov,
                             int iovcnt) override;
    IoAwaitable submit_writev(int fd, const struct iovec* iov,
                              int iovcnt) override;
    IoAwaitable submit_preadv(int fd, const struct iovec* iov, int iovcnt,
                              off_t offset) override;
    IoAwaitable submit_pwritev(int fd, const struct iovec* iov, int iovcnt,
                               off_t offset) override;
    IoAwaitable submit_lseek(int fd, off_t offset, int whence) override;
    IoAwaitable submit_sendfile(int out_fd, int in_fd, off_t offset,
                                std::size_t count) override;

    std::size_t poll(int timeout_ms) override;
    int flush() override;
    std::string name() const override { return "epoll+threadpool"; }

    /// Called by await_suspend via SubmitContext::submit.
    static void submit_to_pool(SubmitContext* ctx, IoAwaitable* awaitable);

   private:
    /// Execute blocking syscall and resume coroutine.
    static void execute_request(IoRequest* req);

    /// Epoll loop run by the completion thread. Currently only watches
    /// the eventfd for shutdown; will be extended for socket I/O.
    void epoll_loop();

    Executor& executor_;
    IoThreadPool pool_;
    IoCompletionThread completion_thread_;
    int epoll_fd_ = -1;
    int event_fd_ = -1;
};

}  // namespace dftracer::utils::io

#endif  // __linux__
