#ifndef DFTRACER_UTILS_CORE_IO_IO_BACKEND_H
#define DFTRACER_UTILS_CORE_IO_IO_BACKEND_H

#include <dftracer/utils/core/io/awaitable.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <string>

namespace dftracer::utils::io {

/// Backend selection preference.
enum class IoBackendType {
    AUTO,  // Runtime detection: io_uring > epoll/kqueue+threadpool > threadpool
    IO_URING,           // Force io_uring (Linux only, fails if unavailable)
    EPOLL_THREADPOOL,   // Force epoll + thread pool (Linux only)
    KQUEUE_THREADPOOL,  // Force kqueue + thread pool (macOS/BSD only)
    THREADPOOL          // Force pure thread pool
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

    /// Submit an async read operation. Returns an IoAwaitable that
    /// will be completed when the read finishes.
    virtual IoAwaitable submit_read(int fd, void *buf, std::size_t len,
                                    off_t offset) = 0;

    /// Submit an async write operation.
    virtual IoAwaitable submit_write(int fd, const void *buf, std::size_t len,
                                     off_t offset) = 0;

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

    /// Non-blocking poll for completions. Called by idle workers.
    /// Returns number of completions reaped.
    virtual std::size_t poll(int timeout_ms = 0) = 0;

    /// Flush all pending batched operations. Backends that submit
    /// ops immediately (thread pool, epoll+threadpool) return 0.
    /// io_uring backend submits all pending SQEs in one syscall.
    /// Returns number of operations flushed.
    virtual int flush() { return 0; }

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

#endif  // DFTRACER_UTILS_CORE_IO_IO_BACKEND_H
