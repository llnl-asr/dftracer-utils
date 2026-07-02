#ifndef DFTRACER_UTILS_CORE_IO_THREAD_POOL_FILE_OPS_H
#define DFTRACER_UTILS_CORE_IO_THREAD_POOL_FILE_OPS_H

#include <dftracer/utils/core/common/object_pool.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/io_op.h>
#include <dftracer/utils/core/io/io_thread_pool.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <cstddef>
#include <string>

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// Request descriptor that doubles as SubmitContext.
/// Heap-allocated per I/O operation, freed after completion.
struct IoRequest : SubmitContext {
    static void* operator new(std::size_t size) {
        return ObjectPool::instance().allocate(size);
    }
    static void operator delete(void* ptr, std::size_t size) {
        ObjectPool::instance().deallocate(ptr, size);
    }

    IoOp op = IoOp::READ;
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
    int msg_flags = 0;
    const struct iovec* iov = nullptr;
    int iovcnt = 0;
    int whence = 0;
    int dest_fd = -1;
    IoAwaitable* awaitable = nullptr;
    IoCompletionFn completion = nullptr;
    void* completion_ctx = nullptr;
    Executor* executor = nullptr;
    IoThreadPool* pool = nullptr;
};

/// Shared base for the thread-pool-backed I/O backends.
/// Owns the executor reference and the I/O thread pool, and implements the
/// full submit_* surface once. Concrete backends (pure thread pool, epoll,
/// kqueue) derive from this and add only their reactor-specific bits
/// (start/stop, name, event loop). io_uring uses a separate SQE-based path.
class ThreadPoolFileOps : public IoBackend {
   public:
    explicit ThreadPoolFileOps(Executor& executor, std::size_t pool_size = 4,
                               unsigned batch_threshold = 0);

    IoAwaitable submit_read(int fd, void* buf, std::size_t len) override;
    IoAwaitable submit_write(int fd, const void* buf, std::size_t len) override;
    IoAwaitable submit_pread(int fd, void* buf, std::size_t len,
                             off_t offset) override;
    void submit_pread_callback(int fd, void* buf, std::size_t len, off_t offset,
                               IoCompletionFn completion,
                               void* context) override;
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

    /// Thread-pool backends complete via callbacks; nothing to reap.
    std::size_t poll(int timeout_ms) override;

    /// Flush pending batched submissions.
    int flush() override;

    /// Called by await_suspend via SubmitContext::submit.
    /// Submits the IoRequest to the thread pool.
    static void submit_to_pool(SubmitContext* ctx, IoAwaitable* awaitable);

    /// Execute the blocking syscall and resume the coroutine. The syscall
    /// dispatch is identical across the thread-pool backends and
    /// platform-aware via #ifdef.
    static void execute_request(IoRequest* req);

    /// Allocate and initialize an IoRequest for the common scalar ops.
    /// Op-specific fields are post-configured by the caller.
    static IoAwaitable make_request(IoOp op, int fd, void* buf, std::size_t len,
                                    off_t offset, const char* path, int flags,
                                    mode_t mode, Executor* executor,
                                    IoThreadPool* pool);

   protected:
    Executor& executor_;
    IoThreadPool pool_;
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_THREAD_POOL_FILE_OPS_H
