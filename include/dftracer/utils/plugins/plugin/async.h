// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H

#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <coroutine>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>

namespace dftracer::utils::plugins {

namespace detail {

/// @see AsyncOp: the only code that names a task the SDK holds.
struct AsyncOpAccess;

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

namespace detail {

/// What `co_await fail(error)` does in a @ref Task: ends it with the error. The
/// task is never resumed.
struct [[nodiscard]] FailAwait {
    dstd::Error error;

    bool await_ready() const noexcept { return false; }
    template <class Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        h.promise().fail(std::move(error));
    }
    void await_resume() const noexcept {}
};

template <class T>
struct ResultAwait;

/// Forwards to a named awaitable by address. GCC 12 copies the reference that
/// await_transform returns, which a move-only awaitable rejects. A temporary
/// is moved into the awaiter, so it lives in the frame until resume.
template <class A>
struct [[nodiscard]] RefAwait {
    A* a;

    bool await_ready() { return a->await_ready(); }
    template <class H>
    decltype(auto) await_suspend(H h) {
        return a->await_suspend(h);
    }
    decltype(auto) await_resume() { return a->await_resume(); }
};

}  // namespace detail

/// A lazy coroutine whose co_awaits hand the host the tasks to wait for: each
/// step of the driven coroutine runs to its next co_await, the host awaits
/// those tasks, then resumes it (see dftu_svc_task::drive).
///
/// A body ends by running off its end. `co_await fail(error)`, or `co_await` on
/// an @ref dstd::Error, fails the task with it. A `co_await` on a @ref
/// dstd::Result gives its value, or ends the task with its error; so does a
/// `co_await` on an @ref AsyncOp whose call the host refused or whose task
/// failed. An ended task is never resumed: the host reports its
/// status and message, then destroys the frame, which runs the destructors of
/// its locals and so gives back every task they still own.
class Task {
   public:
    /// The state the host driver reads and writes between suspends; it is not
    /// part of the plugin API.
    struct promise_type {
        dftu_task* const* wait = nullptr;
        std::uint32_t n_wait = 0;
        bool tolerant = false;
        bool failed = false;
        const dftu_svc_task* svc = nullptr;
        const dftu_plugin_host* host = nullptr;
        dstd::UniquePtr<detail::ProxyCtx> proxy;
        dstd::Error error;

        Task get_return_object() {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }

        /// Ends the task with `e`: the host reports it at the end of this step.
        void fail(dstd::Error e) noexcept {
            error = std::move(e);
            failed = true;
            n_wait = 0;
        }

        detail::FailAwait await_transform(dstd::Error&& e) noexcept {
            return detail::FailAwait{std::move(e)};
        }
        template <class T>
        detail::ResultAwait<T> await_transform(
            dstd::Result<T>&& result) noexcept {
            return detail::ResultAwait<T>{std::move(result)};
        }
        template <class A>
        auto await_transform(A&& awaitable) noexcept {
            if constexpr (std::is_lvalue_reference_v<A>) {
                return detail::RefAwait<std::remove_reference_t<A>>{&awaitable};
            } else {
                return A(std::move(awaitable));
            }
        }
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

/// What `co_await result` does in a @ref Task: the value, or the end of the
/// task with the error.
template <class T>
struct ResultAwait {
    dstd::Result<T> result;

    bool await_ready() const noexcept { return result.has_value(); }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        h.promise().fail(std::move(result).error());
    }
    T await_resume() {
        if constexpr (!std::is_void_v<T>) return *std::move(result);
    }
};

/// The task service of `host`, or NULL when it has none.
inline const dftu_svc_task* task_service(
    const dftu_plugin_host* host) noexcept {
    return host && host->get_service
               ? static_cast<const dftu_svc_task*>(
                     host->get_service(host->h, DFTU_SVC_TASK))
               : nullptr;
}

/// The status of the first failed task, read through the service so its message
/// lands on the ctx; DFTU_OK or DFTU_END when none failed.
inline dftu_status settle(const Task::promise_type& p, dftu_task* const* tasks,
                          std::uint32_t n) noexcept {
    dftu_status first = DFTU_OK;
    for (std::uint32_t i = 0; i < n && first >= 0; ++i)
        first = p.svc->status(&p.proxy->base, p.host->h, tasks[i]);
    return first;
}

/// The awaited form of an op that hands its failure back as a @ref
/// dstd::Result instead of ending the task; see AsyncOp::expected().
template <class A>
class [[nodiscard]] Settled {
   public:
    explicit Settled(A& awaitable) noexcept : a_(awaitable) {}

    bool await_ready() const noexcept { return a_.resolved(); }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        a_.await_suspend(h);
        h.promise().tolerant = true;
    }
    auto await_resume() { return a_.resume_expected(); }

   private:
    A& a_;
};

}  // namespace detail

/// `co_await fail(error)` ends the awaiting @ref Task with `error`. Pass a
/// named error or a call result, not a braced temporary.
inline detail::FailAwait fail(dstd::Error error) noexcept {
    return detail::FailAwait{std::move(error)};
}

/// The awaited form of a call that turns its status into a value, such as a
/// line batch; see fileio.h.
template <class T, class Finish>
class AsyncValue;

/// Owns a started task until it is awaited or released, or the error of a call
/// the host refused to start. `co_await op` hands the task to the host and ends
/// the awaiting @ref Task with the task's error if it failed; the awaited value
/// is the call's @ref dstd::Condition, @ref dstd::Condition
/// "Condition::Unknown" when it succeeded. `co_await op.expected()` gives the
/// failure as a
/// @ref dstd::Result instead. An op dropped without either, for example
/// when its coroutine frame is destroyed, gives its task back to the host.
class [[nodiscard]] AsyncOp {
   public:
    /// An op that never started: awaiting it fails with `error`.
    explicit AsyncOp(dstd::Error error) noexcept
        : error_(std::move(error)), start_failed_(true) {}
    AsyncOp(AsyncOp&& other) noexcept
        : task_(other.task_),
          svc_(other.svc_),
          h_(other.h_),
          error_(std::move(other.error_)),
          start_failed_(other.start_failed_) {
        other.task_ = nullptr;
    }
    AsyncOp(const AsyncOp&) = delete;
    AsyncOp& operator=(const AsyncOp&) = delete;
    AsyncOp& operator=(AsyncOp&&) = delete;
    ~AsyncOp() {
        if (task_ && !handed_ && svc_ && svc_->discard)
            svc_->discard(h_, task_);
    }

    detail::Settled<AsyncOp> expected() & noexcept {
        return detail::Settled<AsyncOp>(*this);
    }
    detail::Settled<AsyncOp> expected() && noexcept {
        return detail::Settled<AsyncOp>(*this);
    }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        p_ = &h.promise();
        if (start_failed_) {
            p_->fail(std::move(error_));
            return;
        }
        handed_ = true;
        p_->wait = &task_;
        p_->n_wait = 1;
    }
    /// @ref dstd::Condition "Condition::Unknown" when the call succeeded, else
    /// its condition; the host already holds its message on the ctx.
    dstd::Condition await_resume() noexcept {
        const dftu_status status = settle_status();
        return status < 0 ? dataframe::error_of(status).condition()
                          : dstd::Condition::Unknown;
    }

    bool resolved() const noexcept { return start_failed_; }
    dstd::Result<void> resume_expected() noexcept {
        if (start_failed_) return std::move(error_);
        return detail::reported(settle_status());
    }

   private:
    friend struct detail::AsyncOpAccess;
    friend class Host;
    template <class T, class Finish>
    friend class AsyncValue;

    /// The status the awaited task ended with; the host already holds its
    /// message on the ctx.
    dftu_status settle_status() noexcept {
        return detail::settle(*p_, &task_, 1);
    }
    AsyncOp(dftu_task* t, const dftu_plugin_host* host) noexcept
        : task_(t),
          svc_(detail::task_service(host)),
          h_(host ? host->h : nullptr) {}

    /// Hand the task over raw, for @ref AsyncAll, Host::await or a `pending`
    /// slot; the caller then owns it. The start error of an op that never
    /// started, so that it reaches the one who awaits it.
    dstd::Result<dftu_task*> release() noexcept {
        if (start_failed_) return std::move(error_);
        handed_ = true;
        return task_;
    }

    dftu_task* task_ = nullptr;
    const dftu_svc_task* svc_ = nullptr;
    void* h_ = nullptr;
    Task::promise_type* p_ = nullptr;
    dstd::Error error_;
    bool start_failed_ = false;
    bool handed_ = false;
};

/// Several tasks awaited together: the host resumes the Task when all of them
/// finished. It takes what the SDK's release_op gives, so an op that never
/// started fails the awaiting @ref Task with its own status and message, and
/// the tasks that did start are given back to the host unrun. `co_await` ends
/// the awaiting Task with the first error; `co_await all.expected()` gives it
/// as a @ref dstd::Result. Owns the tasks until awaited and gives them back to
/// the host when dropped unawaited.
class [[nodiscard]] AsyncAll {
   public:
    AsyncAll(AsyncAll&& other) noexcept
        : tasks_(std::move(other.tasks_)),
          svc_(other.svc_),
          h_(other.h_),
          error_(std::move(other.error_)),
          start_failed_(other.start_failed_) {
        other.tasks_.clear();
    }
    AsyncAll(const AsyncAll&) = delete;
    AsyncAll& operator=(const AsyncAll&) = delete;
    AsyncAll& operator=(AsyncAll&&) = delete;
    ~AsyncAll() {
        if (handed_) return;
        for (dftu_task* t : tasks_) discard(t);
    }

    detail::Settled<AsyncAll> expected() & noexcept {
        return detail::Settled<AsyncAll>(*this);
    }
    detail::Settled<AsyncAll> expected() && noexcept {
        return detail::Settled<AsyncAll>(*this);
    }

    bool await_ready() const noexcept {
        return !start_failed_ && tasks_.empty();
    }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        p_ = &h.promise();
        if (start_failed_) {
            p_->fail(std::move(error_));
            return;
        }
        handed_ = true;
        p_->wait = tasks_.data();
        p_->n_wait = static_cast<std::uint32_t>(tasks_.size());
    }
    /// @ref dstd::Condition "Condition::Unknown" when every task succeeded,
    /// else the condition of the first failure; the host already holds its
    /// message on the ctx.
    dstd::Condition await_resume() noexcept {
        const dftu_status status = settle_status();
        return status < 0 ? dataframe::error_of(status).condition()
                          : dstd::Condition::Unknown;
    }

    bool resolved() const noexcept { return start_failed_ || tasks_.empty(); }
    dstd::Result<void> resume_expected() noexcept {
        if (start_failed_) return std::move(error_);
        return detail::reported(settle_status());
    }

   private:
    friend class Host;

    /// The status the first failed task ended with; the host already holds its
    /// message on the ctx.
    dftu_status settle_status() noexcept {
        return tasks_.empty()
                   ? DFTU_OK
                   : detail::settle(*p_, tasks_.data(),
                                    static_cast<std::uint32_t>(tasks_.size()));
    }
    AsyncAll(std::initializer_list<dstd::Result<dftu_task*>> ts,
             const dftu_plugin_host* host)
        : svc_(detail::task_service(host)), h_(host_h(host)) {
        for (const auto& t : ts) adopt(t);
    }
    AsyncAll(dstd::Vector<dstd::Result<dftu_task*>> ts,
             const dftu_plugin_host* host)
        : svc_(detail::task_service(host)), h_(host_h(host)) {
        for (const auto& t : ts) adopt(t);
    }

    static void* host_h(const dftu_plugin_host* host) noexcept {
        return host ? host->h : nullptr;
    }

    void discard(dftu_task* t) const noexcept {
        if (t && svc_ && svc_->discard) svc_->discard(h_, t);
    }

    void adopt(const dstd::Result<dftu_task*>& t) {
        if (t.has_value()) {
            if (start_failed_)
                discard(*t);
            else
                tasks_.push_back(*t);
        } else if (!start_failed_) {
            error_ = t.error();
            start_failed_ = true;
            for (dftu_task* started : tasks_) discard(started);
            tasks_.clear();
        }
    }

    dstd::Vector<dftu_task*> tasks_;
    const dftu_svc_task* svc_;
    void* h_;
    Task::promise_type* p_ = nullptr;
    dstd::Error error_;
    bool start_failed_ = false;
    bool handed_ = false;
};

namespace detail {

struct AsyncOpAccess {
    static AsyncOp make(dftu_task* task,
                        const dftu_plugin_host* host) noexcept {
        return AsyncOp{task, host};
    }
    static dstd::Result<dftu_task*> release(AsyncOp& op) noexcept {
        return op.release();
    }
};

/// The op of a task a service call started; when the call failed, an op that
/// fails with its status when awaited. `t` is taken by reference: callers write
/// started(h, call(..., &t), t), and the order of argument evaluation is
/// unspecified.
inline AsyncOp async_op(dftu_task* task,
                        const dftu_plugin_host* host) noexcept {
    return AsyncOpAccess::make(task, host);
}

/// The task inside `op`, which the caller then owns; see @ref AsyncOp.
inline dstd::Result<dftu_task*> release_op(AsyncOp& op) noexcept {
    return AsyncOpAccess::release(op);
}

/// @overload release_op
inline dstd::Result<dftu_task*> release_op(AsyncOp&& op) noexcept {
    return AsyncOpAccess::release(op);
}

}  // namespace detail

/// The arguments of @ref Io::openat2, as the kernel's open_how names them. The
/// call copies them.
struct OpenHow {
    std::uint64_t flags = 0;
    std::uint64_t mode = 0;
    std::uint64_t resolve = 0;
};

/// Thin typed wrapper over the io service; each call starts the operation and
/// returns an @ref AsyncOp to `co_await`. A call the host refuses, or a host
/// without the io group, gives an AsyncOp that fails when awaited, as does a
/// failed operation: DFTU_E_NOT_FOUND for a missing file, DFTU_E_IO or
/// DFTU_E_INVALID otherwise, with the system message.
///
/// A path or a name is copied when the call is made. A buffer, a result slot or
/// an out struct the call names must stay valid until the await ends.
class Io {
   public:
    AsyncOp open(dstd::CStringView path, int flags, int mode,
                 int* out_fd) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->open(ctx_, h_->h, path.c_str(), flags, mode, out_fd, &t), t);
    }
    AsyncOp close(int fd) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->close(ctx_, h_->h, fd, &t), t);
    }
    AsyncOp read(int fd, dstd::MutableByteView buf, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->read(ctx_, h_->h, fd, buf.data(), buf.size(), out_n, &t), t);
    }
    AsyncOp write(int fd, dstd::ByteView buf, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->write(ctx_, h_->h, fd, buf.data(), buf.size(), out_n, &t), t);
    }
    AsyncOp pread(int fd, dstd::MutableByteView buf, std::uint64_t off,
                  std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->pread(ctx_, h_->h, fd, buf.data(), buf.size(), off, out_n, &t),
            t);
    }
    AsyncOp pwrite(int fd, dstd::ByteView buf, std::uint64_t off,
                   std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->pwrite(ctx_, h_->h, fd, buf.data(), buf.size(), off,
                                out_n, &t),
                    t);
    }
    AsyncOp fsync(int fd) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->fsync(ctx_, h_->h, fd, &t), t);
    }
    AsyncOp ftruncate(int fd, std::uint64_t len) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->ftruncate(ctx_, h_->h, fd, len, &t), t);
    }
    AsyncOp fstat(int fd, Stat& out) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->fstat(ctx_, h_->h, fd, detail::StatAccess::raw(out), &t), t);
    }
    /// Scatter-gather sequential read; `iov` is borrowed for the await.
    AsyncOp readv(int fd, dstd::Span<const struct iovec> iov,
                  std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->readv(ctx_, h_->h, fd, iov.data(),
                               static_cast<int>(iov.size()), out_n, &t),
                    t);
    }
    /// Scatter-gather sequential write; `iov` is borrowed for the await.
    AsyncOp writev(int fd, dstd::Span<const struct iovec> iov,
                   std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->writev(ctx_, h_->h, fd, iov.data(),
                                static_cast<int>(iov.size()), out_n, &t),
                    t);
    }
    /// Scatter-gather positional read (seekable fds only).
    AsyncOp preadv(int fd, dstd::Span<const struct iovec> iov,
                   std::uint64_t off, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->preadv(ctx_, h_->h, fd, iov.data(),
                                static_cast<int>(iov.size()), off, out_n, &t),
                    t);
    }
    /// Scatter-gather positional write (seekable fds only).
    AsyncOp pwritev(int fd, dstd::Span<const struct iovec> iov,
                    std::uint64_t off, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->pwritev(ctx_, h_->h, fd, iov.data(),
                                 static_cast<int>(iov.size()), off, out_n, &t),
                    t);
    }
    /// Reposition the file offset; `whence` is SEEK_SET/CUR/END. `out_off`
    /// receives the resulting absolute offset.
    AsyncOp lseek(int fd, std::int64_t off, int whence,
                  std::int64_t* out_off) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->lseek(ctx_, h_->h, fd, off, whence, out_off, &t), t);
    }
    /// Zero-copy transfer of `count` bytes from `in_fd` (a regular file) to
    /// `out_fd` starting at `off`; `out_n` receives the bytes sent.
    AsyncOp sendfile(int out_fd, int in_fd, std::uint64_t off,
                     std::uint64_t count, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->sendfile(ctx_, h_->h, out_fd, in_fd, off, count, out_n, &t),
            t);
    }
    /// Accept a connection on a listening socket; `addr`/`addrlen` may be null.
    /// `out_fd` receives the client fd.
    AsyncOp accept(int fd, struct sockaddr* addr, socklen_t* addrlen,
                   int* out_fd) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->accept(ctx_, h_->h, fd, addr, addrlen, out_fd, &t), t);
    }
    /// Socket receive; `flags` are the recv(2) flags.
    AsyncOp recv(int fd, dstd::MutableByteView buf, int flags,
                 std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->recv(ctx_, h_->h, fd, buf.data(), buf.size(), flags,
                              out_n, &t),
                    t);
    }
    /// Socket send; `flags` are the send(2) flags.
    AsyncOp send(int fd, dstd::ByteView buf, int flags,
                 std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->send(ctx_, h_->h, fd, buf.data(), buf.size(), flags,
                              out_n, &t),
                    t);
    }

    /// The calls below follow the table in abi/io.h: paths, names and socket
    /// addresses are copied when called; buffers and result slots stay valid
    /// until the await ends. statx, fallocate, sync_file_range, splice,
    /// openat2, the xattr calls, tee and pipe fail with DFTU_E_UNSUPPORTED off
    /// Linux.
    AsyncOp unlink(dstd::CStringView path) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->unlink(ctx_, h_->h, path.c_str(), &t), t);
    }
    AsyncOp rmdir(dstd::CStringView path) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->rmdir(ctx_, h_->h, path.c_str(), &t), t);
    }
    AsyncOp rename(dstd::CStringView from, dstd::CStringView to) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->rename(ctx_, h_->h, from.c_str(), to.c_str(), &t), t);
    }
    AsyncOp mkdir(dstd::CStringView path, int mode) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->mkdir(ctx_, h_->h, path.c_str(), mode, &t), t);
    }
    AsyncOp fadvise(int fd, std::uint64_t off, std::uint64_t len,
                    int advice) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->fadvise(ctx_, h_->h, fd, off, len, advice, &t), t);
    }
    AsyncOp linkat(int from_dirfd, dstd::CStringView from, int to_dirfd,
                   dstd::CStringView to, int flags) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->linkat(ctx_, h_->h, from_dirfd, from.c_str(), to_dirfd,
                                to.c_str(), flags, &t),
                    t);
    }
    AsyncOp symlinkat(dstd::CStringView target, int dirfd,
                      dstd::CStringView link) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->symlinkat(ctx_, h_->h, target.c_str(), dirfd,
                                   link.c_str(), &t),
                    t);
    }
    AsyncOp socket(int domain, int type, int protocol, int* out_fd) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(
            io_->socket(ctx_, h_->h, domain, type, protocol, out_fd, &t), t);
    }
    AsyncOp connect(int fd, const struct sockaddr* addr,
                    socklen_t addrlen) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->connect(ctx_, h_->h, fd, addr, addrlen, &t), t);
    }
    AsyncOp bind(int fd, const struct sockaddr* addr, socklen_t addrlen) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->bind(ctx_, h_->h, fd, addr, addrlen, &t), t);
    }
    AsyncOp listen(int fd, int backlog) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->listen(ctx_, h_->h, fd, backlog, &t), t);
    }
    AsyncOp shutdown(int fd, int how) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->shutdown(ctx_, h_->h, fd, how, &t), t);
    }
    AsyncOp sendmsg(int fd, const struct msghdr* msg, int flags,
                    std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->sendmsg(ctx_, h_->h, fd, msg, flags, out_n, &t), t);
    }
    AsyncOp recvmsg(int fd, struct msghdr* msg, int flags,
                    std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->recvmsg(ctx_, h_->h, fd, msg, flags, out_n, &t), t);
    }
    AsyncOp poll_add(int fd, std::uint32_t events,
                     std::uint32_t* out_events) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->poll_add(ctx_, h_->h, fd, events, out_events, &t), t);
    }
    AsyncOp statx(int dirfd, dstd::CStringView path, int flags,
                  std::uint32_t mask, Statx& out) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->statx(ctx_, h_->h, dirfd, path.c_str(), flags, mask,
                               detail::StatxAccess::raw(out), &t),
                    t);
    }
    AsyncOp fallocate(int fd, int mode, std::uint64_t off,
                      std::uint64_t len) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->fallocate(ctx_, h_->h, fd, mode, off, len, &t), t);
    }
    AsyncOp sync_file_range(int fd, std::uint64_t off, std::uint64_t len,
                            std::uint32_t flags) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->sync_file_range(ctx_, h_->h, fd, off, len, flags, &t),
                    t);
    }
    AsyncOp splice(int in_fd, std::int64_t* in_off, int out_fd,
                   std::int64_t* out_off, std::uint64_t len,
                   std::uint32_t flags, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->splice(ctx_, h_->h, in_fd, in_off, out_fd, out_off,
                                len, flags, out_n, &t),
                    t);
    }
    AsyncOp openat2(int dirfd, dstd::CStringView path, const OpenHow& how,
                    int* out_fd) const {
        if (!io_) return unsupported();
        const dftu_open_how raw{how.flags, how.mode, how.resolve};
        dftu_task* t = nullptr;
        return done(
            io_->openat2(ctx_, h_->h, dirfd, path.c_str(), &raw, out_fd, &t),
            t);
    }
    AsyncOp getxattr(dstd::CStringView path, dstd::CStringView name,
                     dstd::MutableByteView value, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->getxattr(ctx_, h_->h, path.c_str(), name.c_str(),
                                  value.data(), value.size(), out_n, &t),
                    t);
    }
    AsyncOp setxattr(dstd::CStringView path, dstd::CStringView name,
                     dstd::ByteView value, int flags) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->setxattr(ctx_, h_->h, path.c_str(), name.c_str(),
                                  value.data(), value.size(), flags, &t),
                    t);
    }
    AsyncOp fgetxattr(int fd, dstd::CStringView name,
                      dstd::MutableByteView value, std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->fgetxattr(ctx_, h_->h, fd, name.c_str(), value.data(),
                                   value.size(), out_n, &t),
                    t);
    }
    AsyncOp fsetxattr(int fd, dstd::CStringView name, dstd::ByteView value,
                      int flags) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->fsetxattr(ctx_, h_->h, fd, name.c_str(), value.data(),
                                   value.size(), flags, &t),
                    t);
    }
    AsyncOp tee(int in_fd, int out_fd, std::uint64_t len, std::uint32_t flags,
                std::int64_t* out_n) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->tee(ctx_, h_->h, in_fd, out_fd, len, flags, out_n, &t),
                    t);
    }
    AsyncOp pipe(int fds[2], int flags) const {
        if (!io_) return unsupported();
        dftu_task* t = nullptr;
        return done(io_->pipe(ctx_, h_->h, fds, flags, &t), t);
    }

   private:
    friend class Host;

    Io(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          io_(h && h->get_service ? static_cast<const dftu_io*>(
                                        h->get_service(h->h, DFTU_SVC_IO))
                                  : nullptr) {}

    static AsyncOp unsupported() {
        return AsyncOp{
            make_error(dstd::Condition::Unsupported, "host lacks dftu.svc.io")};
    }
    // `t` by reference: callers write done(call(..., &t), t), and the order of
    // argument evaluation is unspecified, so a copy could predate the call.
    AsyncOp done(dftu_status st, dftu_task* const& t) const {
        if (st < 0) return AsyncOp{dataframe::error_of(st)};
        return detail::async_op(t, h_);
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_io* io_;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_ASYNC_H */
