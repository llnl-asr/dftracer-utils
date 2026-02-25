#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)

#include "kqueue_thread_pool_backend.h"

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "thread_pool_backend.h"  // IoRequest, IoOp

namespace dftracer::utils::io {

KqueueThreadPoolBackend::KqueueThreadPoolBackend(Executor& executor,
                                                 std::size_t pool_size,
                                                 unsigned batch_threshold)
    : executor_(executor), pool_(pool_size, batch_threshold) {}

KqueueThreadPoolBackend::~KqueueThreadPoolBackend() {
    // Ensure cleanup even if stop() was not called.
    if (kqueue_fd_ >= 0) {
        ::close(kqueue_fd_);
        kqueue_fd_ = -1;
    }
}

void KqueueThreadPoolBackend::start() {
    pool_.start();

    kqueue_fd_ = ::kqueue();
    if (kqueue_fd_ < 0) {
        DFTRACER_UTILS_LOG_ERROR("kqueue() failed: %s", std::strerror(errno));
        return;
    }

    // Register a user event (EVFILT_USER) for shutdown signaling.
    struct kevent ev{};
    EV_SET(&ev, SHUTDOWN_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        DFTRACER_UTILS_LOG_ERROR("kevent(register EVFILT_USER) failed: %s",
                                 std::strerror(errno));
    }

    DFTRACER_UTILS_LOG_DEBUG("kqueue+threadpool backend started (kqueue_fd=%d)",
                             kqueue_fd_);

    completion_thread_.start([this] { kqueue_loop(); });
}

void KqueueThreadPoolBackend::stop() {
    // Signal the completion thread to exit.
    completion_thread_.signal_stop();

    // Wake kevent() by triggering the user event.
    if (kqueue_fd_ >= 0) {
        struct kevent ev{};
        EV_SET(&ev, SHUTDOWN_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
    }

    completion_thread_.join();
    pool_.stop();

    if (kqueue_fd_ >= 0) {
        ::close(kqueue_fd_);
        kqueue_fd_ = -1;
    }
}

void KqueueThreadPoolBackend::kqueue_loop() {
    constexpr int MAX_EVENTS = 64;
    struct kevent events[MAX_EVENTS];

    // 100ms timeout for periodic running() check.
    struct timespec timeout{};
    timeout.tv_sec = 0;
    timeout.tv_nsec = 100 * 1000 * 1000;  // 100ms

    while (completion_thread_.running()) {
        int n = ::kevent(kqueue_fd_, nullptr, 0, events, MAX_EVENTS, &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_USER &&
                events[i].ident == SHUTDOWN_IDENT) {
                // Shutdown signal -- process any other events first.
                continue;
            }

            // Future: dispatch socket I/O events here.
            // For now, only the user event is registered.
        }
    }
}

// -- File I/O: identical to ThreadPoolBackend / EpollThreadPoolBackend --

static IoAwaitable make_kqueue_request(IoOp op, int fd, void* buf,
                                       std::size_t len, off_t offset,
                                       const char* path, int flags, mode_t mode,
                                       Executor* executor, IoThreadPool* pool) {
    auto* req = new IoRequest{};
    req->submit = &KqueueThreadPoolBackend::submit_to_pool;
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

IoAwaitable KqueueThreadPoolBackend::submit_read(int fd, void* buf,
                                                 std::size_t len,
                                                 off_t offset) {
    return make_kqueue_request(IoOp::READ, fd, buf, len, offset, nullptr, 0, 0,
                               &executor_, &pool_);
}

IoAwaitable KqueueThreadPoolBackend::submit_write(int fd, const void* buf,
                                                  std::size_t len,
                                                  off_t offset) {
    return make_kqueue_request(IoOp::WRITE, fd, const_cast<void*>(buf), len,
                               offset, nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable KqueueThreadPoolBackend::submit_open(const char* path, int flags,
                                                 mode_t mode) {
    return make_kqueue_request(IoOp::OPEN, -1, nullptr, 0, 0, path, flags, mode,
                               &executor_, &pool_);
}

IoAwaitable KqueueThreadPoolBackend::submit_close(int fd) {
    return make_kqueue_request(IoOp::CLOSE, fd, nullptr, 0, 0, nullptr, 0, 0,
                               &executor_, &pool_);
}

IoAwaitable KqueueThreadPoolBackend::submit_fsync(int fd) {
    return make_kqueue_request(IoOp::FSYNC, fd, nullptr, 0, 0, nullptr, 0, 0,
                               &executor_, &pool_);
}

IoAwaitable KqueueThreadPoolBackend::submit_ftruncate(int fd, off_t length) {
    return make_kqueue_request(IoOp::FTRUNCATE, fd, nullptr, 0, length, nullptr,
                               0, 0, &executor_, &pool_);
}

IoAwaitable KqueueThreadPoolBackend::submit_fstat(int fd, struct stat* buf) {
    auto req_awaitable = make_kqueue_request(IoOp::FSTAT, fd, nullptr, 0, 0,
                                             nullptr, 0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->stat_buf = buf;
    return req_awaitable;
}

void KqueueThreadPoolBackend::submit_to_pool(SubmitContext* ctx,
                                             IoAwaitable* awaitable) {
    auto* req = static_cast<IoRequest*>(ctx);
    req->awaitable = awaitable;
    req->pool->submit([req] { execute_request(req); });
}

void KqueueThreadPoolBackend::execute_request(IoRequest* req) {
    ssize_t result = 0;
    switch (req->op) {
        case IoOp::READ:
            result = ::pread(req->fd, req->buf, req->len, req->offset);
            break;
        case IoOp::WRITE:
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
    }
    if (result < 0) result = -errno;

    req->awaitable->result_ = result;
    req->executor->enqueue(req->awaitable->handle_);
    delete req;
}

std::size_t KqueueThreadPoolBackend::poll(int /*timeout_ms*/) {
    // Completions fire via thread pool callbacks -- nothing to poll.
    return 0;
}

int KqueueThreadPoolBackend::flush() { return static_cast<int>(pool_.flush()); }

}  // namespace dftracer::utils::io

#endif  // kqueue platforms
