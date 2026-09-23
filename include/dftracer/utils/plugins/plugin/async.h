#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H

#include <dftracer/utils/plugins/abi.h>

#include <coroutine>
#include <cstdint>
#include <exception>

namespace dftracer::utils::plugins {

/// A lazy coroutine that yields, at each co_await, the next dftu_task the host
/// should await; the host drives it to completion (see
/// dftu_plugin_host::drive).
class Task {
   public:
    struct promise_type {
        dftu_task* pending = nullptr;
        const dftu_plugin_host* host = nullptr;
        std::exception_ptr exc;

        Task get_return_object() {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { exc = std::current_exception(); }
    };

    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() {
        if (h_) h_.destroy();
    }

    /// Give up ownership; the host driver destroys the frame on completion.
    std::coroutine_handle<promise_type> release() {
        auto h = h_;
        h_ = nullptr;
        return h;
    }

   private:
    std::coroutine_handle<promise_type> h_;
};

/// Suspends the Task and hands `task` to the host as the next thing to await.
/// Decays to dftu_task* so it also passes where a raw task is expected.
struct AsyncOp {
    dftu_task* task;
    bool await_ready() const noexcept { return false; }
    void await_suspend(
        std::coroutine_handle<Task::promise_type> h) const noexcept {
        h.promise().pending = task;
    }
    void await_resume() const noexcept {}
    operator dftu_task*() const noexcept { return task; }
};

/// Thin typed wrapper over the io ext; each call returns an AsyncOp to
/// `co_await`, a no-op if the host does not provide the io group.
class Io {
   public:
    explicit Io(const dftu_plugin_host* h)
        : h_(h),
          io_(h && h->get_service ? static_cast<const dftu_io*>(
                                        h->get_service(h->h, DFTU_SVC_IO))
                                  : nullptr) {}
    AsyncOp open(const char* path, int flags, int mode, int* out_fd) const {
        return AsyncOp{io_ ? io_->open(h_->h, path, flags, mode, out_fd)
                           : nullptr};
    }
    AsyncOp close(int fd, int* out_rc) const {
        return AsyncOp{io_ ? io_->close(h_->h, fd, out_rc) : nullptr};
    }
    AsyncOp read(int fd, void* buf, std::uint64_t len,
                 std::int64_t* out_n) const {
        return AsyncOp{io_ ? io_->read(h_->h, fd, buf, len, out_n) : nullptr};
    }
    AsyncOp write(int fd, const void* buf, std::uint64_t len,
                  std::int64_t* out_n) const {
        return AsyncOp{io_ ? io_->write(h_->h, fd, buf, len, out_n) : nullptr};
    }
    AsyncOp pread(int fd, void* buf, std::uint64_t len, std::uint64_t off,
                  std::int64_t* out_n) const {
        return AsyncOp{io_ ? io_->pread(h_->h, fd, buf, len, off, out_n)
                           : nullptr};
    }
    AsyncOp pwrite(int fd, const void* buf, std::uint64_t len,
                   std::uint64_t off, std::int64_t* out_n) const {
        return AsyncOp{io_ ? io_->pwrite(h_->h, fd, buf, len, off, out_n)
                           : nullptr};
    }
    AsyncOp fsync(int fd, int* out_rc) const {
        return AsyncOp{io_ ? io_->fsync(h_->h, fd, out_rc) : nullptr};
    }
    AsyncOp ftruncate(int fd, std::uint64_t len, int* out_rc) const {
        return AsyncOp{io_ ? io_->ftruncate(h_->h, fd, len, out_rc) : nullptr};
    }
    AsyncOp fstat(int fd, dftu_stat* out) const {
        return AsyncOp{io_ ? io_->fstat(h_->h, fd, out) : nullptr};
    }
    /// Scatter-gather sequential read; `iov`/`iovcnt` borrowed for the await.
    AsyncOp readv(int fd, const struct iovec* iov, int iovcnt,
                  std::int64_t* out_n) const {
        return AsyncOp{io_ && io_->readv
                           ? io_->readv(h_->h, fd, iov, iovcnt, out_n)
                           : nullptr};
    }
    /// Scatter-gather sequential write; `iov`/`iovcnt` borrowed for the await.
    AsyncOp writev(int fd, const struct iovec* iov, int iovcnt,
                   std::int64_t* out_n) const {
        return AsyncOp{io_ && io_->writev
                           ? io_->writev(h_->h, fd, iov, iovcnt, out_n)
                           : nullptr};
    }
    /// Scatter-gather positional read (seekable fds only).
    AsyncOp preadv(int fd, const struct iovec* iov, int iovcnt,
                   std::uint64_t off, std::int64_t* out_n) const {
        return AsyncOp{io_ && io_->preadv
                           ? io_->preadv(h_->h, fd, iov, iovcnt, off, out_n)
                           : nullptr};
    }
    /// Scatter-gather positional write (seekable fds only).
    AsyncOp pwritev(int fd, const struct iovec* iov, int iovcnt,
                    std::uint64_t off, std::int64_t* out_n) const {
        return AsyncOp{io_ && io_->pwritev
                           ? io_->pwritev(h_->h, fd, iov, iovcnt, off, out_n)
                           : nullptr};
    }
    /// Reposition the file offset; `whence` is SEEK_SET/CUR/END. `out_off`
    /// receives the resulting absolute offset.
    AsyncOp lseek(int fd, std::int64_t off, int whence,
                  std::int64_t* out_off) const {
        return AsyncOp{io_ && io_->lseek
                           ? io_->lseek(h_->h, fd, off, whence, out_off)
                           : nullptr};
    }
    /// Zero-copy transfer of `count` bytes from `in_fd` (a regular file) to
    /// `out_fd` starting at `off`; `out_n` receives the bytes sent.
    AsyncOp sendfile(int out_fd, int in_fd, std::uint64_t off,
                     std::uint64_t count, std::int64_t* out_n) const {
        return AsyncOp{
            io_ && io_->sendfile
                ? io_->sendfile(h_->h, out_fd, in_fd, off, count, out_n)
                : nullptr};
    }
    /// Accept a connection on a listening socket; `addr`/`addrlen` may be null.
    /// `out_fd` receives the client fd.
    AsyncOp accept(int fd, struct sockaddr* addr, socklen_t* addrlen,
                   int* out_fd) const {
        return AsyncOp{io_ && io_->accept
                           ? io_->accept(h_->h, fd, addr, addrlen, out_fd)
                           : nullptr};
    }
    /// Socket receive; `flags` are the recv(2) flags.
    AsyncOp recv(int fd, void* buf, std::uint64_t len, int flags,
                 std::int64_t* out_n) const {
        return AsyncOp{io_ && io_->recv
                           ? io_->recv(h_->h, fd, buf, len, flags, out_n)
                           : nullptr};
    }
    /// Socket send; `flags` are the send(2) flags.
    AsyncOp send(int fd, const void* buf, std::uint64_t len, int flags,
                 std::int64_t* out_n) const {
        return AsyncOp{io_ && io_->send
                           ? io_->send(h_->h, fd, buf, len, flags, out_n)
                           : nullptr};
    }

   private:
    const dftu_plugin_host* h_;
    const dftu_io* io_;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H */
