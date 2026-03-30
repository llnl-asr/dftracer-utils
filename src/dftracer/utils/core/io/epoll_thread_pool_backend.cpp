#ifdef __linux__

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/epoll_thread_pool_backend.h>
#include <dftracer/utils/core/io/thread_pool_backend.h>  // IoRequest, IoOp
#include <dftracer/utils/core/pipeline/executor.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dftracer::utils::io {

EpollThreadPoolBackend::EpollThreadPoolBackend(Executor& executor,
                                               std::size_t pool_size,
                                               unsigned batch_threshold)
    : executor_(executor), pool_(pool_size, batch_threshold) {}

EpollThreadPoolBackend::~EpollThreadPoolBackend() {
    // Ensure cleanup even if stop() was not called.
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

void EpollThreadPoolBackend::start() {
    pool_.start();

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        DFTRACER_UTILS_LOG_ERROR("epoll_create1 failed: %s",
                                 std::strerror(errno));
        return;
    }

    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        DFTRACER_UTILS_LOG_ERROR("eventfd failed: %s", std::strerror(errno));
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // Register the eventfd with epoll for shutdown wakeup.
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = event_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0) {
        DFTRACER_UTILS_LOG_ERROR("epoll_ctl(eventfd) failed: %s",
                                 std::strerror(errno));
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "epoll+threadpool backend started (epoll_fd=%d, event_fd=%d)",
        epoll_fd_, event_fd_);

    completion_thread_.start([this] { epoll_loop(); });
}

void EpollThreadPoolBackend::stop() {
    // Signal the completion thread to exit.
    completion_thread_.signal_stop();

    // Wake epoll_wait by writing to eventfd.
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
    }

    completion_thread_.join();
    pool_.stop();

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

void EpollThreadPoolBackend::epoll_loop() {
    constexpr int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (completion_thread_.running()) {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == event_fd_) {
                // Shutdown signal -- drain the eventfd and exit.
                uint64_t val = 0;
                [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
                // Don't break immediately; process any other events first.
                continue;
            }

            // Future: dispatch socket I/O events here.
            // For now, only the eventfd is registered.
        }
    }
}

// -- File I/O: identical to ThreadPoolBackend --

static IoAwaitable make_epoll_request(IoOp op, int fd, void* buf,
                                      std::size_t len, off_t offset,
                                      const char* path, int flags, mode_t mode,
                                      Executor* executor, IoThreadPool* pool) {
    auto* req = new IoRequest{};
    req->submit = &EpollThreadPoolBackend::submit_to_pool;
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
    req->awaitable = nullptr;
    awaitable.submit_ctx_ = req;
    return awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_read(int fd, void* buf,
                                                std::size_t len) {
    return make_epoll_request(IoOp::READ, fd, buf, len, 0, nullptr, 0, 0,
                              &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_write(int fd, const void* buf,
                                                 std::size_t len) {
    return make_epoll_request(IoOp::WRITE, fd, const_cast<void*>(buf), len, 0,
                              nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_pread(int fd, void* buf,
                                                 std::size_t len,
                                                 off_t offset) {
    return make_epoll_request(IoOp::PREAD, fd, buf, len, offset, nullptr, 0, 0,
                              &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_pwrite(int fd, const void* buf,
                                                  std::size_t len,
                                                  off_t offset) {
    return make_epoll_request(IoOp::PWRITE, fd, const_cast<void*>(buf), len,
                              offset, nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_open(const char* path, int flags,
                                                mode_t mode) {
    return make_epoll_request(IoOp::OPEN, -1, nullptr, 0, 0, path, flags, mode,
                              &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_close(int fd) {
    return make_epoll_request(IoOp::CLOSE, fd, nullptr, 0, 0, nullptr, 0, 0,
                              &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_fsync(int fd) {
    return make_epoll_request(IoOp::FSYNC, fd, nullptr, 0, 0, nullptr, 0, 0,
                              &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_ftruncate(int fd, off_t length) {
    return make_epoll_request(IoOp::FTRUNCATE, fd, nullptr, 0, length, nullptr,
                              0, 0, &executor_, &pool_);
}

IoAwaitable EpollThreadPoolBackend::submit_fstat(int fd, struct stat* buf) {
    auto req_awaitable = make_epoll_request(IoOp::FSTAT, fd, nullptr, 0, 0,
                                            nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->stat_buf = buf;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_accept(int listen_fd,
                                                  struct sockaddr* addr,
                                                  socklen_t* addrlen) {
    auto req_awaitable =
        make_epoll_request(IoOp::ACCEPT, listen_fd, nullptr, 0, 0, nullptr, 0,
                           0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->addr = addr;
    req->addrlen = addrlen;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_recv(int fd, void* buf,
                                                std::size_t len, int flags) {
    auto req_awaitable = make_epoll_request(IoOp::RECV, fd, buf, len, 0,
                                            nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->msg_flags = flags;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_send(int fd, const void* buf,
                                                std::size_t len, int flags) {
    auto req_awaitable =
        make_epoll_request(IoOp::SEND, fd, const_cast<void*>(buf), len, 0,
                           nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->msg_flags = flags;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_readv(int fd,
                                                 const struct iovec* iov,
                                                 int iovcnt) {
    auto req_awaitable = make_epoll_request(IoOp::READV, fd, nullptr, 0, 0,
                                            nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_writev(int fd,
                                                  const struct iovec* iov,
                                                  int iovcnt) {
    auto req_awaitable = make_epoll_request(IoOp::WRITEV, fd, nullptr, 0, 0,
                                            nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_preadv(int fd,
                                                  const struct iovec* iov,
                                                  int iovcnt, off_t offset) {
    auto req_awaitable =
        make_epoll_request(IoOp::PREADV, fd, nullptr, 0, offset, nullptr, 0, 0,
                           &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_pwritev(int fd,
                                                   const struct iovec* iov,
                                                   int iovcnt, off_t offset) {
    auto req_awaitable =
        make_epoll_request(IoOp::PWRITEV, fd, nullptr, 0, offset, nullptr, 0, 0,
                           &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->iov = iov;
    req->iovcnt = iovcnt;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_lseek(int fd, off_t offset,
                                                 int whence) {
    auto req_awaitable = make_epoll_request(IoOp::LSEEK, fd, nullptr, 0, offset,
                                            nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->whence = whence;
    return req_awaitable;
}

IoAwaitable EpollThreadPoolBackend::submit_sendfile(int out_fd, int in_fd,
                                                    off_t offset,
                                                    std::size_t count) {
    auto req_awaitable =
        make_epoll_request(IoOp::SENDFILE, in_fd, nullptr, count, offset,
                           nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->dest_fd = out_fd;
    return req_awaitable;
}

void EpollThreadPoolBackend::submit_to_pool(SubmitContext* ctx,
                                            IoAwaitable* awaitable) {
    auto* req = static_cast<IoRequest*>(ctx);
    req->awaitable = awaitable;
    req->pool->submit([req] { execute_request(req); });
}

void EpollThreadPoolBackend::execute_request(IoRequest* req) {
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
            result = ::accept4(req->fd, req->addr, req->addrlen,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
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
        case IoOp::SENDFILE: {
            off_t off = req->offset;
            result = ::sendfile(req->dest_fd, req->fd, &off, req->len);
            break;
        }
    }
    if (result < 0) result = -errno;

    req->awaitable->result_ = result;
    req->executor->enqueue(req->awaitable->handle_);
    delete req;
}

std::size_t EpollThreadPoolBackend::poll(int /*timeout_ms*/) {
    // Completions fire via thread pool callbacks -- nothing to poll.
    return 0;
}

int EpollThreadPoolBackend::flush() { return static_cast<int>(pool_.flush()); }

}  // namespace dftracer::utils::io

#endif  // __linux__
