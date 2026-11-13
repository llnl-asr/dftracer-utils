// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <coroutine>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <string>

namespace dftracer::utils::plugins {

namespace detail {

/// A dftu_ctx that forwards to the ctx of the step the host is running. A
/// coroutine outlives the callback that created it, so the Host it holds points
/// at this stable ctx instead of the callback's own.
struct ProxyCtx {
    dftu_ctx base;
    dftu_ctx* real;

    static dftu_status set_error(dftu_ctx* c, dftu_status code, const char* msg,
                                 std::uint32_t len) {
        dftu_ctx* r = static_cast<ProxyCtx*>(c->h)->real;
        return r->vt->set_error(r, code, msg, len);
    }
    static dftu_status check(dftu_ctx* c) {
        dftu_ctx* r = static_cast<ProxyCtx*>(c->h)->real;
        return r->vt->check(r);
    }
    static const dftu_ctx_vt* vt() {
        static const dftu_ctx_vt VT = {sizeof(dftu_ctx_vt), set_error, check};
        return &VT;
    }

    explicit ProxyCtx(dftu_ctx* r)
        : base{sizeof(dftu_ctx), 0, vt(), this}, real(r) {}
    ProxyCtx(const ProxyCtx&) = delete;
    ProxyCtx& operator=(const ProxyCtx&) = delete;
};

}  // namespace detail

/// A lazy coroutine whose co_awaits hand the host the tasks to wait for: each
/// step of the driven coroutine runs to its next co_await, the host awaits
/// those tasks, then resumes it (see dftu_svc_task::drive). An error thrown in
/// the body, or a failed awaited task, ends it with that error.
class Task {
   public:
    struct promise_type {
        dftu_task* const* wait = nullptr;
        std::uint32_t n_wait = 0;
        const dftu_svc_task* svc = nullptr;
        const dftu_plugin_host* host = nullptr;
        dstd::UniquePtr<detail::ProxyCtx> proxy;
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

namespace detail {

/// The task service of `host`, or NULL when it has none.
inline const dftu_svc_task* task_service(
    const dftu_plugin_host* host) noexcept {
    return host && host->get_service
               ? static_cast<const dftu_svc_task*>(
                     host->get_service(host->h, DFTU_SVC_TASK))
               : nullptr;
}

/// What a co_await returns once the host resumed: the status of the first
/// failed task, read through the service so its message lands on the ctx.
/// Throws it unless `nothrow`.
inline dftu_status settle(const Task::promise_type& p, dftu_task* const* tasks,
                          std::uint32_t n, bool nothrow) {
    dftu_status first = DFTU_OK;
    for (std::uint32_t i = 0; i < n && first >= 0; ++i)
        first = p.svc->status(&p.proxy->base, p.host->h, tasks[i]);
    if (first < 0 && !nothrow) throw Error(first, "awaited task failed", true);
    return first;
}

}  // namespace detail

/// Owns a started task until it is awaited or released. `co_await op` hands it
/// to the host and throws the task's error if it failed; `co_await
/// op.nothrow()` returns its status instead. An op dropped without either, for
/// example while an exception unwinds, gives its task back to the host.
class AsyncOp {
   public:
    AsyncOp(dftu_task* t, const dftu_plugin_host* host) noexcept
        : task_(t),
          svc_(detail::task_service(host)),
          h_(host ? host->h : nullptr) {}
    AsyncOp(AsyncOp&& other) noexcept
        : task_(other.task_),
          svc_(other.svc_),
          h_(other.h_),
          nothrow_(other.nothrow_) {
        other.task_ = nullptr;
    }
    AsyncOp(const AsyncOp&) = delete;
    AsyncOp& operator=(const AsyncOp&) = delete;
    AsyncOp& operator=(AsyncOp&&) = delete;
    ~AsyncOp() {
        if (task_ && !handed_ && svc_ && svc_->discard)
            svc_->discard(h_, task_);
    }

    AsyncOp& nothrow() & noexcept {
        nothrow_ = true;
        return *this;
    }
    AsyncOp&& nothrow() && noexcept {
        nothrow_ = true;
        return static_cast<AsyncOp&&>(*this);
    }

    /// Hand the task over raw, for @ref AsyncAll or a `pending` slot; the
    /// caller then owns it.
    dftu_task* release() noexcept {
        handed_ = true;
        return task_;
    }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        handed_ = true;
        p_ = &h.promise();
        p_->wait = &task_;
        p_->n_wait = 1;
    }
    dftu_status await_resume() {
        return detail::settle(*p_, &task_, 1, nothrow_);
    }

   private:
    dftu_task* task_;
    const dftu_svc_task* svc_;
    void* h_;
    Task::promise_type* p_ = nullptr;
    bool nothrow_ = false;
    bool handed_ = false;
};

/// Several tasks awaited together: the host resumes the Task when all of them
/// finished. `co_await` throws the first error unless `nothrow()`. Owns the
/// tasks until awaited and gives them back to the host when dropped unawaited.
class AsyncAll {
   public:
    AsyncAll(std::initializer_list<dftu_task*> ts,
             const dftu_plugin_host* host) noexcept
        : tasks_(ts), svc_(detail::task_service(host)), h_(host_h(host)) {}
    AsyncAll(dstd::Vector<dftu_task*> ts, const dftu_plugin_host* host) noexcept
        : tasks_(std::move(ts)),
          svc_(detail::task_service(host)),
          h_(host_h(host)) {}
    AsyncAll(AsyncAll&& other) noexcept
        : tasks_(std::move(other.tasks_)),
          svc_(other.svc_),
          h_(other.h_),
          nothrow_(other.nothrow_) {
        other.tasks_.clear();
    }
    AsyncAll(const AsyncAll&) = delete;
    AsyncAll& operator=(const AsyncAll&) = delete;
    AsyncAll& operator=(AsyncAll&&) = delete;
    ~AsyncAll() {
        if (handed_ || !svc_ || !svc_->discard) return;
        for (dftu_task* t : tasks_) {
            if (t) svc_->discard(h_, t);
        }
    }

    AsyncAll& nothrow() & noexcept {
        nothrow_ = true;
        return *this;
    }
    AsyncAll&& nothrow() && noexcept {
        nothrow_ = true;
        return static_cast<AsyncAll&&>(*this);
    }

    bool await_ready() const noexcept { return tasks_.empty(); }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        handed_ = true;
        p_ = &h.promise();
        p_->wait = tasks_.data();
        p_->n_wait = static_cast<std::uint32_t>(tasks_.size());
    }
    dftu_status await_resume() {
        return tasks_.empty()
                   ? DFTU_OK
                   : detail::settle(*p_, tasks_.data(),
                                    static_cast<std::uint32_t>(tasks_.size()),
                                    nothrow_);
    }

   private:
    static void* host_h(const dftu_plugin_host* host) noexcept {
        return host ? host->h : nullptr;
    }

    dstd::Vector<dftu_task*> tasks_;
    const dftu_svc_task* svc_;
    void* h_;
    Task::promise_type* p_ = nullptr;
    bool nothrow_ = false;
    bool handed_ = false;
};

/// Thin typed wrapper over the io service; each call starts the operation and
/// returns an @ref AsyncOp to `co_await`. A call the host refuses, or a host
/// without the io group, throws @ref Error. A failed operation surfaces when
/// the AsyncOp is awaited: DFTU_E_NOT_FOUND for a missing file, DFTU_E_IO or
/// DFTU_E_INVALID otherwise, with the system message.
class Io {
   public:
    Io(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          io_(h && h->get_service ? static_cast<const dftu_io*>(
                                        h->get_service(h->h, DFTU_SVC_IO))
                                  : nullptr) {}

    AsyncOp open(const char* path, int flags, int mode, int* out_fd) const {
        dftu_task* t = nullptr;
        return done(need().open(ctx_, h_->h, path, flags, mode, out_fd, &t), t);
    }
    AsyncOp close(int fd) const {
        dftu_task* t = nullptr;
        return done(need().close(ctx_, h_->h, fd, &t), t);
    }
    AsyncOp read(int fd, void* buf, std::uint64_t len,
                 std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().read(ctx_, h_->h, fd, buf, len, out_n, &t), t);
    }
    AsyncOp write(int fd, const void* buf, std::uint64_t len,
                  std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().write(ctx_, h_->h, fd, buf, len, out_n, &t), t);
    }
    AsyncOp pread(int fd, void* buf, std::uint64_t len, std::uint64_t off,
                  std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().pread(ctx_, h_->h, fd, buf, len, off, out_n, &t), t);
    }
    AsyncOp pwrite(int fd, const void* buf, std::uint64_t len,
                   std::uint64_t off, std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().pwrite(ctx_, h_->h, fd, buf, len, off, out_n, &t),
                    t);
    }
    AsyncOp fsync(int fd) const {
        dftu_task* t = nullptr;
        return done(need().fsync(ctx_, h_->h, fd, &t), t);
    }
    AsyncOp ftruncate(int fd, std::uint64_t len) const {
        dftu_task* t = nullptr;
        return done(need().ftruncate(ctx_, h_->h, fd, len, &t), t);
    }
    AsyncOp fstat(int fd, dftu_stat* out) const {
        dftu_task* t = nullptr;
        return done(need().fstat(ctx_, h_->h, fd, out, &t), t);
    }
    /// Scatter-gather sequential read; `iov`/`iovcnt` borrowed for the await.
    AsyncOp readv(int fd, const struct iovec* iov, int iovcnt,
                  std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().readv(ctx_, h_->h, fd, iov, iovcnt, out_n, &t), t);
    }
    /// Scatter-gather sequential write; `iov`/`iovcnt` borrowed for the await.
    AsyncOp writev(int fd, const struct iovec* iov, int iovcnt,
                   std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().writev(ctx_, h_->h, fd, iov, iovcnt, out_n, &t), t);
    }
    /// Scatter-gather positional read (seekable fds only).
    AsyncOp preadv(int fd, const struct iovec* iov, int iovcnt,
                   std::uint64_t off, std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().preadv(ctx_, h_->h, fd, iov, iovcnt, off, out_n, &t),
                    t);
    }
    /// Scatter-gather positional write (seekable fds only).
    AsyncOp pwritev(int fd, const struct iovec* iov, int iovcnt,
                    std::uint64_t off, std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(
            need().pwritev(ctx_, h_->h, fd, iov, iovcnt, off, out_n, &t), t);
    }
    /// Reposition the file offset; `whence` is SEEK_SET/CUR/END. `out_off`
    /// receives the resulting absolute offset.
    AsyncOp lseek(int fd, std::int64_t off, int whence,
                  std::int64_t* out_off) const {
        dftu_task* t = nullptr;
        return done(need().lseek(ctx_, h_->h, fd, off, whence, out_off, &t), t);
    }
    /// Zero-copy transfer of `count` bytes from `in_fd` (a regular file) to
    /// `out_fd` starting at `off`; `out_n` receives the bytes sent.
    AsyncOp sendfile(int out_fd, int in_fd, std::uint64_t off,
                     std::uint64_t count, std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(
            need().sendfile(ctx_, h_->h, out_fd, in_fd, off, count, out_n, &t),
            t);
    }
    /// Accept a connection on a listening socket; `addr`/`addrlen` may be null.
    /// `out_fd` receives the client fd.
    AsyncOp accept(int fd, struct sockaddr* addr, socklen_t* addrlen,
                   int* out_fd) const {
        dftu_task* t = nullptr;
        return done(need().accept(ctx_, h_->h, fd, addr, addrlen, out_fd, &t),
                    t);
    }
    /// Socket receive; `flags` are the recv(2) flags.
    AsyncOp recv(int fd, void* buf, std::uint64_t len, int flags,
                 std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().recv(ctx_, h_->h, fd, buf, len, flags, out_n, &t),
                    t);
    }
    /// Socket send; `flags` are the send(2) flags.
    AsyncOp send(int fd, const void* buf, std::uint64_t len, int flags,
                 std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().send(ctx_, h_->h, fd, buf, len, flags, out_n, &t),
                    t);
    }

    /// The calls below follow the table in abi/io.h: paths, names and socket
    /// addresses are copied when called; buffers and result slots stay valid
    /// until the await ends. statx, fallocate, sync_file_range, splice,
    /// openat2, the xattr calls, tee and pipe throw @ref Error with
    /// DFTU_E_UNSUPPORTED off Linux.
    AsyncOp unlink(const char* path) const {
        dftu_task* t = nullptr;
        return done(need().unlink(ctx_, h_->h, path, &t), t);
    }
    AsyncOp rmdir(const char* path) const {
        dftu_task* t = nullptr;
        return done(need().rmdir(ctx_, h_->h, path, &t), t);
    }
    AsyncOp rename(const char* from, const char* to) const {
        dftu_task* t = nullptr;
        return done(need().rename(ctx_, h_->h, from, to, &t), t);
    }
    AsyncOp mkdir(const char* path, int mode) const {
        dftu_task* t = nullptr;
        return done(need().mkdir(ctx_, h_->h, path, mode, &t), t);
    }
    AsyncOp fadvise(int fd, std::uint64_t off, std::uint64_t len,
                    int advice) const {
        dftu_task* t = nullptr;
        return done(need().fadvise(ctx_, h_->h, fd, off, len, advice, &t), t);
    }
    AsyncOp linkat(int from_dirfd, const char* from, int to_dirfd,
                   const char* to, int flags) const {
        dftu_task* t = nullptr;
        return done(need().linkat(ctx_, h_->h, from_dirfd, from, to_dirfd, to,
                                  flags, &t),
                    t);
    }
    AsyncOp symlinkat(const char* target, int dirfd, const char* link) const {
        dftu_task* t = nullptr;
        return done(need().symlinkat(ctx_, h_->h, target, dirfd, link, &t), t);
    }
    AsyncOp socket(int domain, int type, int protocol, int* out_fd) const {
        dftu_task* t = nullptr;
        return done(
            need().socket(ctx_, h_->h, domain, type, protocol, out_fd, &t), t);
    }
    AsyncOp connect(int fd, const struct sockaddr* addr,
                    socklen_t addrlen) const {
        dftu_task* t = nullptr;
        return done(need().connect(ctx_, h_->h, fd, addr, addrlen, &t), t);
    }
    AsyncOp bind(int fd, const struct sockaddr* addr, socklen_t addrlen) const {
        dftu_task* t = nullptr;
        return done(need().bind(ctx_, h_->h, fd, addr, addrlen, &t), t);
    }
    AsyncOp listen(int fd, int backlog) const {
        dftu_task* t = nullptr;
        return done(need().listen(ctx_, h_->h, fd, backlog, &t), t);
    }
    AsyncOp shutdown(int fd, int how) const {
        dftu_task* t = nullptr;
        return done(need().shutdown(ctx_, h_->h, fd, how, &t), t);
    }
    AsyncOp sendmsg(int fd, const struct msghdr* msg, int flags,
                    std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().sendmsg(ctx_, h_->h, fd, msg, flags, out_n, &t), t);
    }
    AsyncOp recvmsg(int fd, struct msghdr* msg, int flags,
                    std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().recvmsg(ctx_, h_->h, fd, msg, flags, out_n, &t), t);
    }
    AsyncOp poll_add(int fd, std::uint32_t events,
                     std::uint32_t* out_events) const {
        dftu_task* t = nullptr;
        return done(need().poll_add(ctx_, h_->h, fd, events, out_events, &t),
                    t);
    }
    AsyncOp statx(int dirfd, const char* path, int flags, std::uint32_t mask,
                  dftu_statx* out_st) const {
        dftu_task* t = nullptr;
        return done(
            need().statx(ctx_, h_->h, dirfd, path, flags, mask, out_st, &t), t);
    }
    AsyncOp fallocate(int fd, int mode, std::uint64_t off,
                      std::uint64_t len) const {
        dftu_task* t = nullptr;
        return done(need().fallocate(ctx_, h_->h, fd, mode, off, len, &t), t);
    }
    AsyncOp sync_file_range(int fd, std::uint64_t off, std::uint64_t len,
                            std::uint32_t flags) const {
        dftu_task* t = nullptr;
        return done(
            need().sync_file_range(ctx_, h_->h, fd, off, len, flags, &t), t);
    }
    AsyncOp splice(int in_fd, std::int64_t* in_off, int out_fd,
                   std::int64_t* out_off, std::uint64_t len,
                   std::uint32_t flags, std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(need().splice(ctx_, h_->h, in_fd, in_off, out_fd, out_off,
                                  len, flags, out_n, &t),
                    t);
    }
    AsyncOp openat2(int dirfd, const char* path, const dftu_open_how* how,
                    int* out_fd) const {
        dftu_task* t = nullptr;
        return done(need().openat2(ctx_, h_->h, dirfd, path, how, out_fd, &t),
                    t);
    }
    AsyncOp getxattr(const char* path, const char* name, void* value,
                     std::uint64_t size, std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(
            need().getxattr(ctx_, h_->h, path, name, value, size, out_n, &t),
            t);
    }
    AsyncOp setxattr(const char* path, const char* name, const void* value,
                     std::uint64_t size, int flags) const {
        dftu_task* t = nullptr;
        return done(
            need().setxattr(ctx_, h_->h, path, name, value, size, flags, &t),
            t);
    }
    AsyncOp fgetxattr(int fd, const char* name, void* value, std::uint64_t size,
                      std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(
            need().fgetxattr(ctx_, h_->h, fd, name, value, size, out_n, &t), t);
    }
    AsyncOp fsetxattr(int fd, const char* name, const void* value,
                      std::uint64_t size, int flags) const {
        dftu_task* t = nullptr;
        return done(
            need().fsetxattr(ctx_, h_->h, fd, name, value, size, flags, &t), t);
    }
    AsyncOp tee(int in_fd, int out_fd, std::uint64_t len, std::uint32_t flags,
                std::int64_t* out_n) const {
        dftu_task* t = nullptr;
        return done(
            need().tee(ctx_, h_->h, in_fd, out_fd, len, flags, out_n, &t), t);
    }
    AsyncOp pipe(int fds[2], int flags) const {
        dftu_task* t = nullptr;
        return done(need().pipe(ctx_, h_->h, fds, flags, &t), t);
    }

   private:
    const dftu_io& need() const {
        if (!io_) throw Error(DFTU_E_UNSUPPORTED, "host lacks dftu.svc.io");
        return *io_;
    }
    AsyncOp done(dftu_status st, dftu_task* t) const {
        if (st < 0) throw Error(st, "io call failed", true);
        return AsyncOp{t, h_};
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_io* io_;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H */
