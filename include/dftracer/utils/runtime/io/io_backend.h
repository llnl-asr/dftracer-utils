#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_H

#include <dftracer/utils/runtime/io/awaitable.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>
#include <string>

namespace dftracer::utils::io {

using IoCompletionFn = void (*)(void *context, ssize_t result) noexcept;

/// Backend selection preference.
enum class IoBackendType {
    Auto,     ///< Runtime detection: io_uring > epoll/kqueue+threadpool >
              ///< threadpool
    IoUring,  ///< Force io_uring (Linux only, fails if unavailable)
    EpollThreadpool,   ///< Force epoll + thread pool (Linux only)
    KqueueThreadpool,  ///< Force kqueue + thread pool (macOS/BSD only)
    Threadpool         ///< Force pure thread pool
};

/// Abstract I/O backend interface.
/// Concrete implementations (io_uring, epoll+threadpool, threadpool-only)
/// live entirely in src/ and are never exposed to users.
class IoBackend {
   public:
    virtual ~IoBackend() = default;

    /// Start the backend (completion thread, ring init, etc.)
    virtual void start() = 0;

    /// Stop the backend (join completion thread, drain pending ops)
    virtual void stop() = 0;

    /// Submit an async sequential read. Works on any fd type.
    virtual IoAwaitable submit_read(int fd, void *buf, std::size_t len) = 0;

    /// Submit an async sequential write. Works on any fd type.
    virtual IoAwaitable submit_write(int fd, const void *buf,
                                     std::size_t len) = 0;

    /// Submit an async positional read. Only seekable fds.
    virtual IoAwaitable submit_pread(int fd, void *buf, std::size_t len,
                                     off_t offset) = 0;

    /// Submit an async positional read with a completion callback.
    /// The callback receives either a byte count or a negative errno.
    virtual void submit_pread_callback(int fd, void *buf, std::size_t len,
                                       off_t offset, IoCompletionFn completion,
                                       void *context) = 0;

    /// Submit an async positional write. Only seekable fds.
    virtual IoAwaitable submit_pwrite(int fd, const void *buf, std::size_t len,
                                      off_t offset) = 0;

    /// Submit an async positional write with a completion callback.
    /// The callback receives either a byte count or a negative errno. Unlike a
    /// coroutine, the callback runs on the backend's own thread, so a caller
    /// that needs the write dispatched calls flush() afterwards.
    virtual void submit_pwrite_callback(int fd, const void *buf,
                                        std::size_t len, off_t offset,
                                        IoCompletionFn completion,
                                        void *context) = 0;

    /// Submit an async open operation.
    virtual IoAwaitable submit_open(const char *path, int flags,
                                    mode_t mode) = 0;

    /// Submit an async close operation.
    virtual IoAwaitable submit_close(int fd) = 0;

    /// Submit an async fsync operation.
    virtual IoAwaitable submit_fsync(int fd) = 0;

    /// Submit an async ftruncate operation.
    virtual IoAwaitable submit_ftruncate(int fd, off_t length) = 0;

    /// Submit an async fstat operation.
    virtual IoAwaitable submit_fstat(int fd, struct stat *buf) = 0;

    /// Scatter-gather sequential read. Works on any fd type.
    virtual IoAwaitable submit_readv(int fd, const struct iovec *iov,
                                     int iovcnt) = 0;

    /// Scatter-gather sequential write. Works on any fd type.
    virtual IoAwaitable submit_writev(int fd, const struct iovec *iov,
                                      int iovcnt) = 0;

    /// Scatter-gather positional read. Only seekable fds.
    virtual IoAwaitable submit_preadv(int fd, const struct iovec *iov,
                                      int iovcnt, off_t offset) = 0;

    /// Scatter-gather positional write. Only seekable fds.
    virtual IoAwaitable submit_pwritev(int fd, const struct iovec *iov,
                                       int iovcnt, off_t offset) = 0;

    /// Reposition file offset. Returns new offset.
    virtual IoAwaitable submit_lseek(int fd, off_t offset, int whence) = 0;

    /// Zero-copy transfer from in_fd to out_fd. Returns bytes sent.
    virtual IoAwaitable submit_sendfile(int out_fd, int in_fd, off_t offset,
                                        std::size_t count) = 0;

    /// Non-blocking poll for completions. Called by idle workers.
    /// Returns number of completions reaped.
    virtual std::size_t poll(int timeout_ms = 0) = 0;

    /// Flush all pending batched operations. Backends that submit
    /// ops immediately (thread pool, epoll+threadpool) return 0.
    /// io_uring backend submits all pending SQEs in one syscall.
    /// Returns number of operations flushed.
    virtual int flush() { return 0; }

    /// File operations still in flight. The executor waits for zero before it
    /// stops, so a read that finishes after the workers are gone does not
    /// resume into a destroyed queue.
    virtual std::size_t in_flight() const { return 0; }

    /// Cancel every operation in flight and fail any submitted later. Each
    /// operation completes with -ECANCELED and resumes its waiter as usual,
    /// so in_flight() falls to zero without waiting for the peer of a pipe or
    /// socket. Sticky until stop(); thread-safe. Called by executor shutdown.
    virtual void cancel_pending() {}

    /// Accept a connection on a listening socket.
    /// Returns the new client fd via IoAwaitable::result_.
    virtual IoAwaitable submit_accept(int listen_fd, struct sockaddr *addr,
                                      socklen_t *addrlen) = 0;

    /// Receive data from a connected socket.
    /// Returns bytes received via IoAwaitable::result_.
    virtual IoAwaitable submit_recv(int fd, void *buf, std::size_t len,
                                    int flags) = 0;

    /// Send data to a connected socket.
    /// Returns bytes sent via IoAwaitable::result_.
    virtual IoAwaitable submit_send(int fd, const void *buf, std::size_t len,
                                    int flags) = 0;

    /// Blocking read -- for contexts that cannot co_await (e.g., VFS).
    /// Submits through the normal async path and blocks until done.
    ssize_t submit_read_sync(int fd, void *buf, std::size_t len, off_t offset);

    /// Blocking write.
    ssize_t submit_write_sync(int fd, const void *buf, std::size_t len,
                              off_t offset);

    /// Blocking fsync.
    int submit_fsync_sync(int fd);

    /// Blocking ftruncate.
    int submit_ftruncate_sync(int fd, off_t length);

    /// Blocking fstat.
    int submit_fstat_sync(int fd, struct stat *buf);

    /// Human-readable name for logging.
    virtual std::string name() const = 0;
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_H
