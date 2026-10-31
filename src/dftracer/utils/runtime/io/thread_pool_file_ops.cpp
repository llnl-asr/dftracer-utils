#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/io/io_sendfile.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>

namespace dftracer::utils::io {

namespace {
// A socket call may wait on a peer indefinitely, so it is not tracked.
bool is_file_op(IoOp op) {
    return op != IoOp::Accept && op != IoOp::Recv && op != IoOp::Send;
}

// Operations whose syscall can block on a peer for ever, so cancel_pending()
// has to be able to interrupt their wait. File writes are left out: they do
// not wait for a peer and the extra poll would slow every sequential write.
bool waits_for_peer(IoOp op, short& events) {
    switch (op) {
        case IoOp::Read:
        case IoOp::Readv:
        case IoOp::Recv:
        case IoOp::Accept:
            events = POLLIN;
            return true;
        case IoOp::Send:
            events = POLLOUT;
            return true;
        default:
            return false;
    }
}

// A regular file never waits for a peer, so a read of one skips the cancellable
// wait. The type is looked up once per descriptor. The entry is dropped when
// the descriptor is closed or opened through this backend; one closed and
// reused behind its back keeps the old answer.
constexpr int FD_KIND_SLOTS = 4096;
constexpr std::uint8_t FD_UNKNOWN = 0;
constexpr std::uint8_t FD_REGULAR = 1;
constexpr std::uint8_t FD_OTHER = 2;
std::atomic<std::uint8_t> g_fd_kind[FD_KIND_SLOTS];

bool is_regular_file(int fd) {
    if (fd < 0 || fd >= FD_KIND_SLOTS) return false;
    std::uint8_t kind = g_fd_kind[fd].load(std::memory_order_relaxed);
    if (kind == FD_UNKNOWN) {
        struct stat st;
        kind = (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? FD_REGULAR
                                                              : FD_OTHER;
        g_fd_kind[fd].store(kind, std::memory_order_relaxed);
    }
    return kind == FD_REGULAR;
}

void forget_fd_kind(int fd) {
    if (fd >= 0 && fd < FD_KIND_SLOTS) {
        g_fd_kind[fd].store(FD_UNKNOWN, std::memory_order_relaxed);
    }
}

// False when the pool was cancelled before the descriptor became ready. An
// error or hang-up counts as ready: the syscall then reports it.
bool wait_ready(int fd, short events, const IoThreadPool& pool) {
    // poll() ignores a negative descriptor and would wait out every slice.
    if (fd < 0) return true;
    constexpr int SLICE_MS = 20;
    struct pollfd pfd{fd, events, 0};
    while (!pool.cancelling()) {
        const int n = ::poll(&pfd, 1, SLICE_MS);
        if (n != 0) return true;
    }
    return false;
}
}  // namespace

ThreadPoolFileOps::ThreadPoolFileOps(Executor& executor, std::size_t pool_size,
                                     unsigned batch_threshold)
    : executor_(executor), pool_(pool_size, batch_threshold) {}

IoAwaitable ThreadPoolFileOps::make_request(IoOp op, int fd, void* buf,
                                            std::size_t len, off_t offset,
                                            const char* path, int flags,
                                            mode_t mode, Executor* executor,
                                            IoThreadPool* pool) {
    auto* req = new IoRequest{};
    req->submit = &ThreadPoolFileOps::submit_to_pool;
    req->op = op;
    req->fd = fd;
    req->buf = buf;
    req->len = len;
    req->offset = offset;
    req->path = path;
    req->flags = flags;
    req->mode = mode;
    req->executor = executor;
    req->pool = pool;

    IoAwaitable awaitable;
    // req->awaitable will be updated in submit_to_pool with the
    // stable address from await_suspend (the IoAwaitable may move
    // before the coroutine frame is allocated).
    req->awaitable = nullptr;
    awaitable.submit_ctx_ = req;
    return awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_read(int fd, void* buf, std::size_t len) {
    return make_request(IoOp::Read, fd, buf, len, 0, nullptr, 0, 0, &executor_,
                        &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_write(int fd, const void* buf,
                                            std::size_t len) {
    return make_request(IoOp::Write, fd, const_cast<void*>(buf), len, 0,
                        nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_pread(int fd, void* buf, std::size_t len,
                                            off_t offset) {
    return make_request(IoOp::Pread, fd, buf, len, offset, nullptr, 0, 0,
                        &executor_, &pool_);
}

void ThreadPoolFileOps::submit_pread_callback(int fd, void* buf,
                                              std::size_t len, off_t offset,
                                              IoCompletionFn completion,
                                              void* context) {
    auto* req = new IoRequest{};
    req->op = IoOp::Pread;
    req->fd = fd;
    req->buf = buf;
    req->len = len;
    req->offset = offset;
    req->completion = completion;
    req->completion_ctx = context;
    req->pool = &pool_;
    if (is_file_op(req->op)) pool_.begin_tracked();
    pool_.submit([req] { execute_request(req); });
}

IoAwaitable ThreadPoolFileOps::submit_pwrite(int fd, const void* buf,
                                             std::size_t len, off_t offset) {
    return make_request(IoOp::Pwrite, fd, const_cast<void*>(buf), len, offset,
                        nullptr, 0, 0, &executor_, &pool_);
}

void ThreadPoolFileOps::submit_pwrite_callback(int fd, const void* buf,
                                               std::size_t len, off_t offset,
                                               IoCompletionFn completion,
                                               void* context) {
    auto* req = new IoRequest{};
    req->op = IoOp::Pwrite;
    req->fd = fd;
    req->buf = const_cast<void*>(buf);
    req->len = len;
    req->offset = offset;
    req->completion = completion;
    req->completion_ctx = context;
    req->pool = &pool_;
    if (is_file_op(req->op)) pool_.begin_tracked();
    pool_.submit([req] { execute_request(req); });
}

IoAwaitable ThreadPoolFileOps::submit_open(const char* path, int flags,
                                           mode_t mode) {
    return make_request(IoOp::Open, -1, nullptr, 0, 0, path, flags, mode,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_close(int fd) {
    return make_request(IoOp::Close, fd, nullptr, 0, 0, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_fsync(int fd) {
    return make_request(IoOp::Fsync, fd, nullptr, 0, 0, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_ftruncate(int fd, off_t length) {
    return make_request(IoOp::Ftruncate, fd, nullptr, 0, length, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_fstat(int fd, struct stat* buf) {
    auto req_awaitable = make_request(IoOp::Fstat, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->stat_buf = buf;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_accept(int listen_fd,
                                             struct sockaddr* addr,
                                             socklen_t* addrlen) {
    auto req_awaitable = make_request(IoOp::Accept, listen_fd, nullptr, 0, 0,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->addr = addr;
    req->addrlen = addrlen;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_recv(int fd, void* buf, std::size_t len,
                                           int flags) {
    auto req_awaitable = make_request(IoOp::Recv, fd, buf, len, 0, nullptr, 0,
                                      0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->msg_flags = flags;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_send(int fd, const void* buf,
                                           std::size_t len, int flags) {
    auto req_awaitable =
        make_request(IoOp::Send, fd, const_cast<void*>(buf), len, 0, nullptr, 0,
                     0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->msg_flags = flags;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_readv(int fd, const struct iovec* iov,
                                            int iovcnt) {
    auto req_awaitable = make_request(IoOp::Readv, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_writev(int fd, const struct iovec* iov,
                                             int iovcnt) {
    auto req_awaitable = make_request(IoOp::Writev, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_preadv(int fd, const struct iovec* iov,
                                             int iovcnt, off_t offset) {
    auto req_awaitable = make_request(IoOp::Preadv, fd, nullptr, 0, offset,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_pwritev(int fd, const struct iovec* iov,
                                              int iovcnt, off_t offset) {
    auto req_awaitable = make_request(IoOp::Pwritev, fd, nullptr, 0, offset,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_lseek(int fd, off_t offset, int whence) {
    auto req_awaitable = make_request(IoOp::Lseek, fd, nullptr, 0, offset,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->whence = whence;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_sendfile(int out_fd, int in_fd,
                                               off_t offset,
                                               std::size_t count) {
    auto req_awaitable =
        make_request(IoOp::Sendfile, in_fd, nullptr, count, offset, nullptr, 0,
                     0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->dest_fd = out_fd;
    return req_awaitable;
}

void ThreadPoolFileOps::submit_to_pool(SubmitContext* ctx,
                                       IoAwaitable* awaitable) {
    auto* req = static_cast<IoRequest*>(ctx);
    if (is_file_op(req->op)) req->pool->begin_tracked();
    // Update the awaitable pointer -- await_suspend passes the real,
    // stable address of the IoAwaitable in the coroutine frame.
    req->awaitable = awaitable;
    req->pool->submit([req] { execute_request(req); });
}

namespace {

// The raw syscall result: a count or fd, or -1 with errno set.
ssize_t run_syscall(const IoRequest* req) {
    ssize_t result = 0;
    switch (req->op) {
        case IoOp::Read:
            result = ::read(req->fd, req->buf, req->len);
            break;
        case IoOp::Write:
            result = ::write(req->fd, req->buf, req->len);
            break;
        case IoOp::Pread:
            result = ::pread(req->fd, req->buf, req->len, req->offset);
            break;
        case IoOp::Pwrite:
            result = ::pwrite(req->fd, req->buf, req->len, req->offset);
            break;
        case IoOp::Open:
            result = ::open(req->path, req->flags, req->mode);
            break;
        case IoOp::Close:
            result = ::close(req->fd);
            break;
        case IoOp::Fsync:
            result = ::fsync(req->fd);
            break;
        case IoOp::Ftruncate:
            result = ::ftruncate(req->fd, req->offset);
            break;
        case IoOp::Fstat:
            result = ::fstat(req->fd, req->stat_buf);
            break;
        case IoOp::Accept:
#ifdef __linux__
            result = ::accept4(req->fd, req->addr, req->addrlen,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
            result = ::accept(req->fd, req->addr, req->addrlen);
            if (result >= 0) {
                int fd = static_cast<int>(result);
                int fl = ::fcntl(fd, F_GETFL, 0);
                ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
#endif
            break;
        case IoOp::Recv:
            result = ::recv(req->fd, req->buf, req->len, req->msg_flags);
            break;
        case IoOp::Send:
            result = ::send(req->fd, req->buf, req->len, req->msg_flags);
            break;
        case IoOp::Readv:
            result = ::readv(req->fd, req->iov, req->iovcnt);
            break;
        case IoOp::Writev:
            result = ::writev(req->fd, req->iov, req->iovcnt);
            break;
        case IoOp::Preadv:
            result = ::preadv(req->fd, req->iov, req->iovcnt, req->offset);
            break;
        case IoOp::Pwritev:
            result = ::pwritev(req->fd, req->iov, req->iovcnt, req->offset);
            break;
        case IoOp::Lseek:
            result = ::lseek(req->fd, req->offset, req->whence);
            break;
        case IoOp::Sendfile:
            result =
                platform_sendfile(req->dest_fd, req->fd, req->offset, req->len);
            break;
    }
    return result;
}

}  // namespace

void ThreadPoolFileOps::execute_request(IoRequest* req) {
    ssize_t result = 0;
    short events = 0;
    const bool may_wait = waits_for_peer(req->op, events) &&
                          !((req->op == IoOp::Read || req->op == IoOp::Readv) &&
                            is_regular_file(req->fd));
    if (may_wait && !wait_ready(req->fd, events, *req->pool)) {
        result = -ECANCELED;
    } else {
        result = run_syscall(req);
        if (result < 0) result = -errno;
        if (req->op == IoOp::Close) {
            forget_fd_kind(req->fd);
        } else if (req->op == IoOp::Open && result >= 0) {
            forget_fd_kind(static_cast<int>(result));
        }
    }

    IoThreadPool* const pool = req->pool;
    const bool tracked = is_file_op(req->op);
    if (req->awaitable != nullptr) {
        req->awaitable->result_ = result;
        req->executor->enqueue(req->awaitable->handle_);
    } else if (req->completion != nullptr) {
        req->completion(req->completion_ctx, result);
    }
    delete req;
    if (tracked) pool->end_tracked();
}

std::size_t ThreadPoolFileOps::poll(int /*timeout_ms*/) {
    return pool_.flush();
}

int ThreadPoolFileOps::flush() { return static_cast<int>(pool_.flush()); }

}  // namespace dftracer::utils::io
