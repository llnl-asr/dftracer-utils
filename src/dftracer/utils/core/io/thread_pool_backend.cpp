#include "thread_pool_backend.h"

#include <dftracer/utils/core/pipeline/executor.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace dftracer::utils::io {

ThreadPoolBackend::ThreadPoolBackend(Executor& executor, std::size_t pool_size,
                                     unsigned batch_threshold)
    : executor_(executor), pool_(pool_size, batch_threshold) {}

void ThreadPoolBackend::start() { pool_.start(); }
void ThreadPoolBackend::stop() { pool_.stop(); }

static IoAwaitable make_request(IoOp op, int fd, void* buf, std::size_t len,
                                off_t offset, const char* path, int flags,
                                mode_t mode, Executor* executor,
                                IoThreadPool* pool) {
    auto* req = new IoRequest{};
    req->submit = &ThreadPoolBackend::submit_to_pool;
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

IoAwaitable ThreadPoolBackend::submit_read(int fd, void* buf, std::size_t len,
                                           off_t offset) {
    return make_request(IoOp::READ, fd, buf, len, offset, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolBackend::submit_write(int fd, const void* buf,
                                            std::size_t len, off_t offset) {
    return make_request(IoOp::WRITE, fd, const_cast<void*>(buf), len, offset,
                        nullptr, 0, 0, &executor_, &pool_);
}

IoAwaitable ThreadPoolBackend::submit_open(const char* path, int flags,
                                           mode_t mode) {
    return make_request(IoOp::OPEN, -1, nullptr, 0, 0, path, flags, mode,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolBackend::submit_close(int fd) {
    return make_request(IoOp::CLOSE, fd, nullptr, 0, 0, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolBackend::submit_fsync(int fd) {
    return make_request(IoOp::FSYNC, fd, nullptr, 0, 0, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolBackend::submit_ftruncate(int fd, off_t length) {
    return make_request(IoOp::FTRUNCATE, fd, nullptr, 0, length, nullptr, 0, 0,
                        &executor_, &pool_);
}

IoAwaitable ThreadPoolBackend::submit_fstat(int fd, struct stat* buf) {
    auto req_awaitable = make_request(IoOp::FSTAT, fd, nullptr, 0, 0, nullptr,
                                      0, 0, &executor_, &pool_);
    auto* req = static_cast<IoRequest*>(req_awaitable.submit_ctx_);
    req->stat_buf = buf;
    return req_awaitable;
}

void ThreadPoolBackend::submit_to_pool(SubmitContext* ctx,
                                       IoAwaitable* awaitable) {
    auto* req = static_cast<IoRequest*>(ctx);
    // Update the awaitable pointer -- await_suspend passes the real,
    // stable address of the IoAwaitable in the coroutine frame.
    req->awaitable = awaitable;
    req->pool->submit([req] { execute_request(req); });
}

void ThreadPoolBackend::execute_request(IoRequest* req) {
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

std::size_t ThreadPoolBackend::poll(int /*timeout_ms*/) {
    return 0;  // Thread pool backend: completions fire via callbacks
}

int ThreadPoolBackend::flush() { return static_cast<int>(pool_.flush()); }

std::string ThreadPoolBackend::name() const { return "threadpool"; }

}  // namespace dftracer::utils::io