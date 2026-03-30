#ifndef DFTRACER_UTILS_CORE_IO_IO_URING_BACKEND_H
#define DFTRACER_UTILS_CORE_IO_IO_URING_BACKEND_H
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/core/common/object_pool.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/io_completion_thread.h>
#include <dftracer/utils/core/io/io_uring_wrapper.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <cstddef>
#include <mutex>
#include <string>

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// io_uring request. Stored in the SQE's user_data so we can
/// recover the IoAwaitable pointer on completion.
struct IoUringRequest {
    static void* operator new(std::size_t size) {
        return ObjectPool::instance().allocate(size);
    }
    static void operator delete(void* ptr, std::size_t size) {
        ObjectPool::instance().deallocate(ptr, size);
    }

    IoAwaitable* awaitable = nullptr;
};

/// io_uring I/O backend using raw syscalls (no liburing dependency).
/// Uses a dedicated completion thread that blocks on wait_cqe and
/// enqueues completed coroutine handles back to the executor.
class IoUringBackend : public IoBackend {
   public:
    explicit IoUringBackend(Executor& executor, unsigned ring_entries = 256,
                            unsigned batch_threshold = 16);

    /// Probe whether io_uring actually works on this kernel at runtime.
    /// Returns true if io_uring_setup succeeds.
    bool probe();

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
    std::string name() const override { return "io_uring"; }

    /// Static callback for SubmitContext::submit.
    static void submit_fn(SubmitContext* ctx, IoAwaitable* awaitable);

   private:
    /// Completion loop run by the completion thread.
    void completion_loop();

    /// Flush pending SQEs if threshold reached. Called under submit_mutex_.
    void maybe_flush_locked();

    Executor& executor_;
    unsigned ring_entries_;
    unsigned batch_threshold_;
    uring::Ring ring_;
    IoCompletionThread completion_thread_;
    std::mutex submit_mutex_;
};

/// SubmitContext subclass for io_uring. Carries the operation
/// details needed to prepare an SQE on await_suspend.
struct IoUringSubmitCtx : SubmitContext {
    static void* operator new(std::size_t size) {
        return ObjectPool::instance().allocate(size);
    }
    static void operator delete(void* ptr, std::size_t size) {
        ObjectPool::instance().deallocate(ptr, size);
    }

    enum class Op {
        READ,
        WRITE,
        PREAD,
        PWRITE,
        OPEN,
        CLOSE,
        FSYNC,
        FTRUNCATE,
        FSTAT,
        ACCEPT,
        RECV,
        SEND,
        READV,
        WRITEV,
        PREADV,
        PWRITEV,
        LSEEK,
        SENDFILE
    };
    Op op = Op::READ;
    int fd = -1;
    void* buf = nullptr;
    std::size_t len = 0;
    off_t offset = 0;
    const char* path = nullptr;
    int flags = 0;
    mode_t mode = 0;
    struct stat* stat_buf = nullptr;
    struct sockaddr* addr = nullptr;
    socklen_t* addrlen = nullptr;
    int accept_flags = 0;
    int msg_flags = 0;
    const struct iovec* iov = nullptr;
    int iovcnt = 0;
    int whence = 0;
    int dest_fd = -1;
    IoUringBackend* backend = nullptr;
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_HAVE_IO_URING
#endif  // DFTRACER_UTILS_CORE_IO_IO_URING_BACKEND_H
