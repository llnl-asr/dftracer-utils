#include <dftracer/utils/core/io/io_sendfile.h>
#include <dftracer/utils/core/io/thread_pool_file_ops.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>

namespace dftracer::utils::io {

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
    return make_request(IoOp::READ, fd, buf, len, 0, nullptr, 0, 0, &executor_,
                        &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_write(int fd, const void* buf,
                                            std::size_t len) {
    return make_request(IoOp::WRITE, fd, const_cast<void*>(buf), len, 0,
                        nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_pread(int fd, void* buf, std::size_t len,
                                            off_t offset) {
    return make_request(IoOp::PREAD, fd, buf, len, offset, nullptr, 0, 0,
                        &executor_, &pool_);
}

void ThreadPoolFileOps::submit_pread_callback(int fd, void* buf,
                                              std::size_t len, off_t offset,
                                              IoCompletionFn completion,
                                              void* context) {
    auto* req = new IoRequest{};
    req->op = IoOp::PREAD;
    req->fd = fd;
    req->buf = buf;
    req->len = len;
    req->offset = offset;
    req->completion = completion;
    req->completion_ctx = context;
    req->pool = &pool_;
    pool_.submit([req] { execute_request(req); });
}

IoAwaitable ThreadPoolFileOps::submit_pwrite(int fd, const void* buf,
                                             std::size_t len, off_t offset) {
    return make_request(IoOp::PWRITE, fd, const_cast<void*>(buf), len, offset,
                        nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_open(const char* path, int flags,
                                           mode_t mode) {
    return make_request(IoOp::OPEN, -1, nullptr, 0, 0, path, flags, mode,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_close(int fd) {
    return make_request(IoOp::CLOSE, fd, nullptr, 0, 0, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_fsync(int fd) {
    return make_request(IoOp::FSYNC, fd, nullptr, 0, 0, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_ftruncate(int fd, off_t length) {
    return make_request(IoOp::FTRUNCATE, fd, nullptr, 0, length, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolFileOps::submit_fstat(int fd, struct stat* buf) {
    auto req_awaitable = make_request(IoOp::FSTAT, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->stat_buf = buf;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_accept(int listen_fd,
                                             struct sockaddr* addr,
                                             socklen_t* addrlen) {
    auto req_awaitable = make_request(IoOp::ACCEPT, listen_fd, nullptr, 0, 0,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->addr = addr;
    req->addrlen = addrlen;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_recv(int fd, void* buf, std::size_t len,
                                           int flags) {
    auto req_awaitable = make_request(IoOp::RECV, fd, buf, len, 0, nullptr, 0,
                                      0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->msg_flags = flags;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_send(int fd, const void* buf,
                                           std::size_t len, int flags) {
    auto req_awaitable =
        make_request(IoOp::SEND, fd, const_cast<void*>(buf), len, 0, nullptr, 0,
                     0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->msg_flags = flags;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_readv(int fd, const struct iovec* iov,
                                            int iovcnt) {
    auto req_awaitable = make_request(IoOp::READV, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_writev(int fd, const struct iovec* iov,
                                             int iovcnt) {
    auto req_awaitable = make_request(IoOp::WRITEV, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_preadv(int fd, const struct iovec* iov,
                                             int iovcnt, off_t offset) {
    auto req_awaitable = make_request(IoOp::PREADV, fd, nullptr, 0, offset,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_pwritev(int fd, const struct iovec* iov,
                                              int iovcnt, off_t offset) {
    auto req_awaitable = make_request(IoOp::PWRITEV, fd, nullptr, 0, offset,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_lseek(int fd, off_t offset, int whence) {
    auto req_awaitable = make_request(IoOp::LSEEK, fd, nullptr, 0, offset,
                                      nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->whence = whence;
    return req_awaitable;
}

IoAwaitable ThreadPoolFileOps::submit_sendfile(int out_fd, int in_fd,
                                               off_t offset,
                                               std::size_t count) {
    auto req_awaitable =
        make_request(IoOp::SENDFILE, in_fd, nullptr, count, offset, nullptr, 0,
                     0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->dest_fd = out_fd;
    return req_awaitable;
}

void ThreadPoolFileOps::submit_to_pool(SubmitContext* ctx,
                                       IoAwaitable* awaitable) {
    auto* req = static_cast<IoRequest*>(ctx);
    // Update the awaitable pointer -- await_suspend passes the real,
    // stable address of the IoAwaitable in the coroutine frame.
    req->awaitable = awaitable;
    req->pool->submit([req] { execute_request(req); });
}

void ThreadPoolFileOps::execute_request(IoRequest* req) {
    ssize_t result = 0;
    switch (req->op) {
        case IoOp::READ:
            result = ::read(req->fd, req->buf, req->len);
            break;
        case IoOp::WRITE:
            result = ::write(req->fd, req->buf, req->len);
            break;
        case IoOp::PREAD:
            result = ::pread(req->fd, req->buf, req->len, req->offset);
            break;
        case IoOp::PWRITE:
            result = ::pwrite(req->fd, req->buf, req->len, req->offset);
            break;
        case IoOp::OPEN:
            result = ::open(req->path, req->flags, req->mode);
            break;
        case IoOp::CLOSE:
            result = ::close(req->fd);
            break;
        case IoOp::FSYNC:
            result = ::fsync(req->fd);
            break;
        case IoOp::FTRUNCATE:
            result = ::ftruncate(req->fd, req->offset);
            break;
        case IoOp::FSTAT:
            result = ::fstat(req->fd, req->stat_buf);
            break;
        case IoOp::ACCEPT:
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
        case IoOp::RECV:
            result = ::recv(req->fd, req->buf, req->len, req->msg_flags);
            break;
        case IoOp::SEND:
            result = ::send(req->fd, req->buf, req->len, req->msg_flags);
            break;
        case IoOp::READV:
            result = ::readv(req->fd, req->iov, req->iovcnt);
            break;
        case IoOp::WRITEV:
            result = ::writev(req->fd, req->iov, req->iovcnt);
            break;
        case IoOp::PREADV:
            result = ::preadv(req->fd, req->iov, req->iovcnt, req->offset);
            break;
        case IoOp::PWRITEV:
            result = ::pwritev(req->fd, req->iov, req->iovcnt, req->offset);
            break;
        case IoOp::LSEEK:
            result = ::lseek(req->fd, req->offset, req->whence);
            break;
        case IoOp::SENDFILE:
            result =
                platform_sendfile(req->dest_fd, req->fd, req->offset, req->len);
            break;
    }
    if (result < 0) result = -errno;

    if (req->awaitable != nullptr) {
        req->awaitable->result_ = result;
        req->executor->enqueue(req->awaitable->handle_);
    } else if (req->completion != nullptr) {
        req->completion(req->completion_ctx, result);
    }
    delete req;
}

std::size_t ThreadPoolFileOps::poll(int /*timeout_ms*/) {
    return pool_.flush();
}

int ThreadPoolFileOps::flush() { return static_cast<int>(pool_.flush()); }

}  // namespace dftracer::utils::io
