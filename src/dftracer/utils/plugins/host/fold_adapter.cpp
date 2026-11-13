// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/field_ref.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/fold_adapter/state.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/trace/arrow/arrow_export.h>
#include <dftracer/utils/trace/arrow/column_builder.h>
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/arrow/ipc_reader.h>
#include <dftracer/utils/trace/arrow/ipc_writer.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::plugins::host {

namespace {

using duql::Query;
using trace::RecordPhase;

const char* host_resolve(void* h, dftu_str id, std::uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (id == DFTU_STR_NONE ||
        id >= dftracer::utils::dstd::StringIntern::FAST_CAPACITY)
        return nullptr;
    auto& intern = static_cast<PluginFold*>(h)->intern_table();
    dstd::StringView sv = intern.resolve(id);
    if (out_size) *out_size = static_cast<std::uint32_t>(sv.size());
    // Interned bytes are stable for the scan, so they outlive the call.
    return sv.data();
}

dftu_str host_intern(void* h, const char* s, std::uint32_t len) {
    if (!s) return DFTU_STR_NONE;
    auto& intern = static_cast<PluginFold*>(h)->intern_table();
    return intern.get_or_insert(dstd::StringView{s, len});
}

// Route a plugin's log line through our logger, tagged with the plugin name, so
// it honors the configured (compile-time and runtime) level.
void host_log(void* h, std::uint8_t level, const char* s, std::uint32_t n) {
    const auto* pf = static_cast<const PluginFold*>(h);
    const char* name =
        pf && !pf->plugin_name().empty() ? pf->plugin_name().c_str() : "plugin";
    const int len = static_cast<int>(n);
    const char* msg = s ? s : "";
    switch (level) {
        case DFTU_LOG_ERROR:
            DFTRACER_UTILS_LOG_ERROR("[plugin:{}] {}", name,
                                     dstd::StringView(msg, len));
            break;
        case DFTU_LOG_WARN:
            DFTRACER_UTILS_LOG_WARN("[plugin:{}] {}", name,
                                    dstd::StringView(msg, len));
            break;
        case DFTU_LOG_DEBUG:
            DFTRACER_UTILS_LOG_DEBUG("[plugin:{}] {}", name,
                                     dstd::StringView(msg, len));
            break;
        case DFTU_LOG_TRACE:
            DFTRACER_UTILS_LOG_TRACE("[plugin:{}] {}", name,
                                     dstd::StringView(msg, len));
            break;
        default:
            DFTRACER_UTILS_LOG_INFO("[plugin:{}] {}", name,
                                    dstd::StringView(msg, len));
            break;
    }
}

// A negative result is -errno from the I/O layer; it fails the task as
// DFTU_E_IO (DFTU_E_NOT_FOUND for ENOENT, DFTU_E_INVALID for EINVAL and EBADF,
// DFTU_E_UNSUPPORTED for ENOSYS, EOPNOTSUPP and ENOTSUP).
bool io_ok(plugins::AbiTask* task, const char* op, std::int64_t result) {
    if (result >= 0) return true;
    const int err = static_cast<int>(-result);
    dftu_status status = DFTU_E_IO;
    if (err == ENOENT) {
        status = DFTU_E_NOT_FOUND;
    } else if (err == EINVAL || err == EBADF) {
        status = DFTU_E_INVALID;
    } else if (err == ENOSYS || err == EOPNOTSUPP
#if ENOTSUP != EOPNOTSUPP
               || err == ENOTSUP
#endif
    ) {
        status = DFTU_E_UNSUPPORTED;
    }
    const dstd::String text = dstd::fmt<"{}: {} (errno {})">(
        dstd::StringView(op), dstd::StringView(std::strerror(err)), err);
    task->fail(status, text);
    return false;
}

// Runs one I/O call and stores its non-negative result in `slot`, if any.
template <class Slot, class Make>
coro::CoroTask<void> io_body(plugins::AbiTask* task, const char* op, Slot* slot,
                             Make make) {
    const std::int64_t result = static_cast<std::int64_t>(co_await make());
    if (io_ok(task, op, result) && slot != nullptr)
        *slot = static_cast<Slot>(result);
}

template <class Make>
dftu_status start_task(::dftu_ctx* ctx, ::dftu_task** out, Make&& make) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "the task slot is NULL");
    *out = plugins::make_abi_task(std::forward<Make>(make));
    return DFTU_OK;
}

// The caller's result slot must outlive the task.
template <class Slot, class Make>
dftu_status io_start(::dftu_ctx* ctx, ::dftu_task** out, const char* op,
                     Slot* slot, Make make) {
    return start_task(ctx, out, [=](plugins::AbiTask* task) {
        return io_body<Slot>(task, op, slot, make);
    });
}

dftu_status io_open(::dftu_ctx* ctx, void*, const char* path,
                    std::int32_t flags, std::int32_t mode, std::int32_t* out_fd,
                    ::dftu_task** out) {
    if (path == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "open: path is NULL");
    // The path is copied into the task: the plugin's buffer need not outlive
    // the call.
    const dstd::String copy(path);
    return io_start(ctx, out, "open", out_fd, [=] {
        return runtime::io::open(copy.c_str(), flags,
                                 static_cast<mode_t>(mode));
    });
}

dftu_status io_close(::dftu_ctx* ctx, void*, std::int32_t fd,
                     ::dftu_task** out) {
    return io_start(ctx, out, "close", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::close(fd); });
}

dftu_status io_read(::dftu_ctx* ctx, void*, std::int32_t fd, void* buf,
                    std::uint64_t len, std::int64_t* out_n, ::dftu_task** out) {
    return io_start(ctx, out, "read", out_n, [=] {
        return runtime::io::read(fd, buf, static_cast<std::size_t>(len));
    });
}

dftu_status io_write(::dftu_ctx* ctx, void*, std::int32_t fd, const void* buf,
                     std::uint64_t len, std::int64_t* out_n,
                     ::dftu_task** out) {
    return io_start(ctx, out, "write", out_n, [=] {
        return runtime::io::write(fd, buf, static_cast<std::size_t>(len));
    });
}

dftu_status io_pread(::dftu_ctx* ctx, void*, std::int32_t fd, void* buf,
                     std::uint64_t len, std::uint64_t off, std::int64_t* out_n,
                     ::dftu_task** out) {
    return io_start(ctx, out, "pread", out_n, [=] {
        return runtime::io::pread(fd, buf, static_cast<std::size_t>(len), off);
    });
}

dftu_status io_pwrite(::dftu_ctx* ctx, void*, std::int32_t fd, const void* buf,
                      std::uint64_t len, std::uint64_t off, std::int64_t* out_n,
                      ::dftu_task** out) {
    return io_start(ctx, out, "pwrite", out_n, [=] {
        return runtime::io::pwrite(fd, buf, static_cast<std::size_t>(len), off);
    });
}

dftu_status io_fsync(::dftu_ctx* ctx, void*, std::int32_t fd,
                     ::dftu_task** out) {
    return io_start(ctx, out, "fsync", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::fsync(fd); });
}

dftu_status io_ftruncate(::dftu_ctx* ctx, void*, std::int32_t fd,
                         std::uint64_t len, ::dftu_task** out) {
    return io_start(ctx, out, "ftruncate", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::ftruncate(fd, len); });
}

coro::CoroTask<void> fstat_body(plugins::AbiTask* task, std::int32_t fd,
                                dftu_stat* out) {
    struct stat st{};
    const ssize_t rc = co_await runtime::io::fstat(fd, &st);
    if (!io_ok(task, "fstat", rc) || out == nullptr) co_return;
    out->size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    const struct timespec& mt = st.st_mtimespec;
#else
    const struct timespec& mt = st.st_mtim;
#endif
    out->mtime_ns = static_cast<std::uint64_t>(mt.tv_sec) * 1000000000ull +
                    static_cast<std::uint64_t>(mt.tv_nsec);
    out->mode = static_cast<std::uint32_t>(st.st_mode);
}

dftu_status io_fstat(::dftu_ctx* ctx, void*, std::int32_t fd, dftu_stat* out_st,
                     ::dftu_task** out) {
    return start_task(ctx, out, [=](plugins::AbiTask* task) {
        return fstat_body(task, fd, out_st);
    });
}

dftu_status io_readv(::dftu_ctx* ctx, void*, std::int32_t fd,
                     const struct iovec* iov, std::int32_t iovcnt,
                     std::int64_t* out_n, ::dftu_task** out) {
    return io_start(ctx, out, "readv", out_n,
                    [=] { return runtime::io::readv(fd, iov, iovcnt); });
}

dftu_status io_writev(::dftu_ctx* ctx, void*, std::int32_t fd,
                      const struct iovec* iov, std::int32_t iovcnt,
                      std::int64_t* out_n, ::dftu_task** out) {
    return io_start(ctx, out, "writev", out_n,
                    [=] { return runtime::io::writev(fd, iov, iovcnt); });
}

dftu_status io_preadv(::dftu_ctx* ctx, void*, std::int32_t fd,
                      const struct iovec* iov, std::int32_t iovcnt,
                      std::uint64_t off, std::int64_t* out_n,
                      ::dftu_task** out) {
    return io_start(ctx, out, "preadv", out_n,
                    [=] { return runtime::io::preadv(fd, iov, iovcnt, off); });
}

dftu_status io_pwritev(::dftu_ctx* ctx, void*, std::int32_t fd,
                       const struct iovec* iov, std::int32_t iovcnt,
                       std::uint64_t off, std::int64_t* out_n,
                       ::dftu_task** out) {
    return io_start(ctx, out, "pwritev", out_n,
                    [=] { return runtime::io::pwritev(fd, iov, iovcnt, off); });
}

dftu_status io_lseek(::dftu_ctx* ctx, void*, std::int32_t fd, std::int64_t off,
                     std::int32_t whence, std::int64_t* out_off,
                     ::dftu_task** out) {
    return io_start(ctx, out, "lseek", out_off,
                    [=] { return runtime::io::lseek(fd, off, whence); });
}

dftu_status io_sendfile(::dftu_ctx* ctx, void*, std::int32_t out_fd,
                        std::int32_t in_fd, std::uint64_t off,
                        std::uint64_t count, std::int64_t* out_n,
                        ::dftu_task** out) {
    return io_start(ctx, out, "sendfile", out_n, [=] {
        return runtime::io::sendfile(out_fd, in_fd, off,
                                     static_cast<std::size_t>(count));
    });
}

dftu_status io_accept(::dftu_ctx* ctx, void*, std::int32_t fd,
                      struct sockaddr* addr, socklen_t* addrlen,
                      std::int32_t* out_fd, ::dftu_task** out) {
    return io_start(ctx, out, "accept", out_fd,
                    [=] { return runtime::io::accept(fd, addr, addrlen); });
}

dftu_status io_recv(::dftu_ctx* ctx, void*, std::int32_t fd, void* buf,
                    std::uint64_t len, std::int32_t flags, std::int64_t* out_n,
                    ::dftu_task** out) {
    return io_start(ctx, out, "recv", out_n, [=] {
        return runtime::io::recv(fd, buf, static_cast<std::size_t>(len), flags);
    });
}

dftu_status io_send(::dftu_ctx* ctx, void*, std::int32_t fd, const void* buf,
                    std::uint64_t len, std::int32_t flags, std::int64_t* out_n,
                    ::dftu_task** out) {
    return io_start(ctx, out, "send", out_n, [=] {
        return runtime::io::send(fd, buf, static_cast<std::size_t>(len), flags);
    });
}

dftu_status io_unlink(::dftu_ctx* ctx, void*, const char* path,
                      ::dftu_task** out) {
    if (path == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "unlink: path is NULL");
    const dstd::String copy(path);
    return io_start(ctx, out, "unlink", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::unlink(copy.c_str()); });
}

dftu_status io_rmdir(::dftu_ctx* ctx, void*, const char* path,
                     ::dftu_task** out) {
    if (path == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "rmdir: path is NULL");
    const dstd::String copy(path);
    return io_start(ctx, out, "rmdir", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::rmdir(copy.c_str()); });
}

dftu_status io_rename(::dftu_ctx* ctx, void*, const char* from, const char* to,
                      ::dftu_task** out) {
    if (from == nullptr || to == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "rename: a path is NULL");
    const dstd::String from_copy(from);
    const dstd::String to_copy(to);
    return io_start(
        ctx, out, "rename", static_cast<std::int64_t*>(nullptr), [=] {
            return runtime::io::rename(from_copy.c_str(), to_copy.c_str());
        });
}

dftu_status io_mkdir(::dftu_ctx* ctx, void*, const char* path,
                     std::int32_t mode, ::dftu_task** out) {
    if (path == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "mkdir: path is NULL");
    const dstd::String copy(path);
    return io_start(
        ctx, out, "mkdir", static_cast<std::int64_t*>(nullptr), [=] {
            return runtime::io::mkdir(copy.c_str(), static_cast<mode_t>(mode));
        });
}

dftu_status io_fadvise(::dftu_ctx* ctx, void*, std::int32_t fd,
                       std::uint64_t off, std::uint64_t len,
                       std::int32_t advice, ::dftu_task** out) {
    return io_start(ctx, out, "fadvise", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::fadvise(fd, off, len, advice); });
}

dftu_status io_linkat(::dftu_ctx* ctx, void*, std::int32_t from_dirfd,
                      const char* from, std::int32_t to_dirfd, const char* to,
                      std::int32_t flags, ::dftu_task** out) {
    if (from == nullptr || to == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "linkat: a path is NULL");
    const dstd::String from_copy(from);
    const dstd::String to_copy(to);
    return io_start(
        ctx, out, "linkat", static_cast<std::int64_t*>(nullptr), [=] {
            return runtime::io::linkat(from_dirfd, from_copy.c_str(), to_dirfd,
                                       to_copy.c_str(), flags);
        });
}

dftu_status io_symlinkat(::dftu_ctx* ctx, void*, const char* target,
                         std::int32_t dirfd, const char* link,
                         ::dftu_task** out) {
    if (target == nullptr || link == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "symlinkat: a path is NULL");
    const dstd::String target_copy(target);
    const dstd::String link_copy(link);
    return io_start(ctx, out, "symlinkat", static_cast<std::int64_t*>(nullptr),
                    [=] {
                        return runtime::io::symlinkat(target_copy.c_str(),
                                                      dirfd, link_copy.c_str());
                    });
}

dftu_status io_socket(::dftu_ctx* ctx, void*, std::int32_t domain,
                      std::int32_t type, std::int32_t protocol,
                      std::int32_t* out_fd, ::dftu_task** out) {
    return io_start(ctx, out, "socket", out_fd, [=] {
        return runtime::io::socket(domain, type, protocol);
    });
}

// The address is copied into the task: the plugin's buffer need not outlive
// the call.
bool copy_sockaddr(::dftu_ctx* ctx, const char* op, const struct sockaddr* addr,
                   socklen_t addrlen, struct sockaddr_storage* out) {
    if (addr == nullptr || addrlen > sizeof(*out)) {
        host_fail(ctx, DFTU_E_INVALID,
                  dstd::fmt<"{}: the socket address is NULL or too long">(
                      dstd::StringView(op)));
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    std::memcpy(out, addr, addrlen);
    return true;
}

dftu_status io_connect(::dftu_ctx* ctx, void*, std::int32_t fd,
                       const struct sockaddr* addr, socklen_t addrlen,
                       ::dftu_task** out) {
    struct sockaddr_storage ss;
    if (!copy_sockaddr(ctx, "connect", addr, addrlen, &ss))
        return DFTU_E_INVALID;
    return io_start(
        ctx, out, "connect", static_cast<std::int64_t*>(nullptr), [=] {
            return runtime::io::connect(
                fd, reinterpret_cast<const struct sockaddr*>(&ss), addrlen);
        });
}

dftu_status io_bind(::dftu_ctx* ctx, void*, std::int32_t fd,
                    const struct sockaddr* addr, socklen_t addrlen,
                    ::dftu_task** out) {
    struct sockaddr_storage ss;
    if (!copy_sockaddr(ctx, "bind", addr, addrlen, &ss)) return DFTU_E_INVALID;
    return io_start(ctx, out, "bind", static_cast<std::int64_t*>(nullptr), [=] {
        return runtime::io::bind(
            fd, reinterpret_cast<const struct sockaddr*>(&ss), addrlen);
    });
}

dftu_status io_listen(::dftu_ctx* ctx, void*, std::int32_t fd,
                      std::int32_t backlog, ::dftu_task** out) {
    return io_start(ctx, out, "listen", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::listen(fd, backlog); });
}

dftu_status io_shutdown(::dftu_ctx* ctx, void*, std::int32_t fd,
                        std::int32_t how, ::dftu_task** out) {
    return io_start(ctx, out, "shutdown", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::shutdown(fd, how); });
}

dftu_status io_sendmsg(::dftu_ctx* ctx, void*, std::int32_t fd,
                       const struct msghdr* msg, std::int32_t flags,
                       std::int64_t* out_n, ::dftu_task** out) {
    if (msg == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "sendmsg: msg is NULL");
    return io_start(ctx, out, "sendmsg", out_n,
                    [=] { return runtime::io::sendmsg(fd, msg, flags); });
}

dftu_status io_recvmsg(::dftu_ctx* ctx, void*, std::int32_t fd,
                       struct msghdr* msg, std::int32_t flags,
                       std::int64_t* out_n, ::dftu_task** out) {
    if (msg == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "recvmsg: msg is NULL");
    return io_start(ctx, out, "recvmsg", out_n,
                    [=] { return runtime::io::recvmsg(fd, msg, flags); });
}

dftu_status io_poll_add(::dftu_ctx* ctx, void*, std::int32_t fd,
                        std::uint32_t events, std::uint32_t* out_events,
                        ::dftu_task** out) {
    return io_start(ctx, out, "poll_add", out_events,
                    [=] { return runtime::io::poll_add(fd, events); });
}

#ifdef __linux__

std::int64_t timestamp_ns(const runtime::io::StatxTimestamp& t) {
    return t.sec * 1000000000ll + static_cast<std::int64_t>(t.nsec);
}

coro::CoroTask<void> statx_body(plugins::AbiTask* task, std::int32_t dirfd,
                                dstd::String path, std::int32_t flags,
                                std::uint32_t mask, dftu_statx* out) {
    runtime::io::Statx st{};
    const ssize_t rc =
        co_await runtime::io::statx(dirfd, path.c_str(), flags, mask, &st);
    if (!io_ok(task, "statx", rc) || out == nullptr) co_return;
    out->attributes = st.attributes;
    out->ino = st.ino;
    out->size = st.size;
    out->blocks = st.blocks;
    out->attributes_mask = st.attributes_mask;
    out->atime_ns = timestamp_ns(st.atime);
    out->btime_ns = timestamp_ns(st.btime);
    out->ctime_ns = timestamp_ns(st.ctime);
    out->mtime_ns = timestamp_ns(st.mtime);
    out->mask = st.mask;
    out->blksize = st.blksize;
    out->nlink = st.nlink;
    out->uid = st.uid;
    out->gid = st.gid;
    out->mode = st.mode;
    out->rdev_major = st.rdev_major;
    out->rdev_minor = st.rdev_minor;
    out->dev_major = st.dev_major;
    out->dev_minor = st.dev_minor;
}

dftu_status io_statx(::dftu_ctx* ctx, void*, std::int32_t dirfd,
                     const char* path, std::int32_t flags, std::uint32_t mask,
                     dftu_statx* out_st, ::dftu_task** out) {
    if (path == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "statx: path is NULL");
    dstd::String copy(path);
    return start_task(ctx, out, [=](plugins::AbiTask* task) {
        return statx_body(task, dirfd, copy, flags, mask, out_st);
    });
}

dftu_status io_fallocate(::dftu_ctx* ctx, void*, std::int32_t fd,
                         std::int32_t mode, std::uint64_t off,
                         std::uint64_t len, ::dftu_task** out) {
    return io_start(ctx, out, "fallocate", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::fallocate(fd, mode, off, len); });
}

dftu_status io_sync_file_range(::dftu_ctx* ctx, void*, std::int32_t fd,
                               std::uint64_t off, std::uint64_t len,
                               std::uint32_t flags, ::dftu_task** out) {
    return io_start(
        ctx, out, "sync_file_range", static_cast<std::int64_t*>(nullptr),
        [=] { return runtime::io::sync_file_range(fd, off, len, flags); });
}

coro::CoroTask<void> splice_body(plugins::AbiTask* task, std::int32_t in_fd,
                                 std::int64_t* in_off, std::int32_t out_fd,
                                 std::int64_t* out_off, std::uint64_t len,
                                 std::uint32_t flags, std::int64_t* out_n) {
    const std::int64_t from = in_off != nullptr ? *in_off : -1;
    const std::int64_t to = out_off != nullptr ? *out_off : -1;
    const std::int64_t result =
        static_cast<std::int64_t>(co_await runtime::io::splice(
            in_fd, from, out_fd, to, static_cast<std::size_t>(len), flags));
    if (!io_ok(task, "splice", result)) co_return;
    if (in_off != nullptr) *in_off += result;
    if (out_off != nullptr) *out_off += result;
    if (out_n != nullptr) *out_n = result;
}

dftu_status io_splice(::dftu_ctx* ctx, void*, std::int32_t in_fd,
                      std::int64_t* in_off, std::int32_t out_fd,
                      std::int64_t* out_off, std::uint64_t len,
                      std::uint32_t flags, std::int64_t* out_n,
                      ::dftu_task** out) {
    return start_task(ctx, out, [=](plugins::AbiTask* task) {
        return splice_body(task, in_fd, in_off, out_fd, out_off, len, flags,
                           out_n);
    });
}

coro::CoroTask<void> openat2_body(plugins::AbiTask* task, std::int32_t dirfd,
                                  dstd::String path, dftu_open_how how,
                                  std::int32_t* out_fd) {
    const runtime::io::OpenHow open_how{how.flags, how.mode, how.resolve};
    const std::int64_t result = static_cast<std::int64_t>(
        co_await runtime::io::openat2(dirfd, path.c_str(), open_how));
    if (io_ok(task, "openat2", result) && out_fd != nullptr)
        *out_fd = static_cast<std::int32_t>(result);
}

dftu_status io_openat2(::dftu_ctx* ctx, void*, std::int32_t dirfd,
                       const char* path, const dftu_open_how* how,
                       std::int32_t* out_fd, ::dftu_task** out) {
    if (path == nullptr || how == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "openat2: path or how is NULL");
    const dstd::String copy(path);
    const dftu_open_how how_copy = *how;
    return start_task(ctx, out, [=](plugins::AbiTask* task) {
        return openat2_body(task, dirfd, copy, how_copy, out_fd);
    });
}

enum class XattrKind { Get, Set, FGet, FSet };

// The name and the arguments live in the frame until the await ends.
coro::CoroTask<void> xattr_body(plugins::AbiTask* task, XattrKind kind,
                                const char* op, dstd::String path,
                                std::int32_t fd, dstd::String name, void* value,
                                std::uint64_t size, std::int32_t flags,
                                std::int64_t* out_n) {
    const runtime::io::XattrArgs args{name.c_str(), value,
                                      static_cast<std::size_t>(size), flags};
    std::int64_t result = 0;
    switch (kind) {
        case XattrKind::Get:
            result = static_cast<std::int64_t>(
                co_await runtime::io::getxattr(path.c_str(), args));
            break;
        case XattrKind::Set:
            result = static_cast<std::int64_t>(
                co_await runtime::io::setxattr(path.c_str(), args));
            break;
        case XattrKind::FGet:
            result = static_cast<std::int64_t>(
                co_await runtime::io::fgetxattr(fd, args));
            break;
        case XattrKind::FSet:
            result = static_cast<std::int64_t>(
                co_await runtime::io::fsetxattr(fd, args));
            break;
    }
    if (io_ok(task, op, result) && out_n != nullptr) *out_n = result;
}

dftu_status xattr_start(::dftu_ctx* ctx, XattrKind kind, const char* op,
                        const char* path, std::int32_t fd, const char* name,
                        void* value, std::uint64_t size, std::int32_t flags,
                        std::int64_t* out_n, ::dftu_task** out) {
    const bool by_path = kind == XattrKind::Get || kind == XattrKind::Set;
    if (name == nullptr || (by_path && path == nullptr)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"{}: the path or name is NULL">(dstd::StringView(op)));
    }
    const dstd::String path_copy(by_path ? path : "");
    const dstd::String name_copy(name);
    return start_task(ctx, out, [=](plugins::AbiTask* task) {
        return xattr_body(task, kind, op, path_copy, fd, name_copy, value, size,
                          flags, out_n);
    });
}

dftu_status io_getxattr(::dftu_ctx* ctx, void*, const char* path,
                        const char* name, void* value, std::uint64_t size,
                        std::int64_t* out_n, ::dftu_task** out) {
    return xattr_start(ctx, XattrKind::Get, "getxattr", path, -1, name, value,
                       size, 0, out_n, out);
}

dftu_status io_setxattr(::dftu_ctx* ctx, void*, const char* path,
                        const char* name, const void* value, std::uint64_t size,
                        std::int32_t flags, ::dftu_task** out) {
    return xattr_start(ctx, XattrKind::Set, "setxattr", path, -1, name,
                       const_cast<void*>(value), size, flags, nullptr, out);
}

dftu_status io_fgetxattr(::dftu_ctx* ctx, void*, std::int32_t fd,
                         const char* name, void* value, std::uint64_t size,
                         std::int64_t* out_n, ::dftu_task** out) {
    return xattr_start(ctx, XattrKind::FGet, "fgetxattr", nullptr, fd, name,
                       value, size, 0, out_n, out);
}

dftu_status io_fsetxattr(::dftu_ctx* ctx, void*, std::int32_t fd,
                         const char* name, const void* value,
                         std::uint64_t size, std::int32_t flags,
                         ::dftu_task** out) {
    return xattr_start(ctx, XattrKind::FSet, "fsetxattr", nullptr, fd, name,
                       const_cast<void*>(value), size, flags, nullptr, out);
}

dftu_status io_tee(::dftu_ctx* ctx, void*, std::int32_t in_fd,
                   std::int32_t out_fd, std::uint64_t len, std::uint32_t flags,
                   std::int64_t* out_n, ::dftu_task** out) {
    return io_start(ctx, out, "tee", out_n, [=] {
        return runtime::io::tee(in_fd, out_fd, static_cast<std::size_t>(len),
                                flags);
    });
}

dftu_status io_pipe(::dftu_ctx* ctx, void*, std::int32_t fds[2],
                    std::int32_t flags, ::dftu_task** out) {
    if (fds == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "pipe: fds is NULL");
    return io_start(ctx, out, "pipe", static_cast<std::int64_t*>(nullptr),
                    [=] { return runtime::io::pipe(fds, flags); });
}

#else

dftu_status io_unsupported(::dftu_ctx* ctx, const char* op) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"{}: not supported on this platform">(dstd::StringView(op)));
}

dftu_status io_statx(::dftu_ctx* ctx, void*, std::int32_t, const char*,
                     std::int32_t, std::uint32_t, dftu_statx*, ::dftu_task**) {
    return io_unsupported(ctx, "statx");
}

dftu_status io_fallocate(::dftu_ctx* ctx, void*, std::int32_t, std::int32_t,
                         std::uint64_t, std::uint64_t, ::dftu_task**) {
    return io_unsupported(ctx, "fallocate");
}

dftu_status io_sync_file_range(::dftu_ctx* ctx, void*, std::int32_t,
                               std::uint64_t, std::uint64_t, std::uint32_t,
                               ::dftu_task**) {
    return io_unsupported(ctx, "sync_file_range");
}

dftu_status io_splice(::dftu_ctx* ctx, void*, std::int32_t, std::int64_t*,
                      std::int32_t, std::int64_t*, std::uint64_t, std::uint32_t,
                      std::int64_t*, ::dftu_task**) {
    return io_unsupported(ctx, "splice");
}

dftu_status io_openat2(::dftu_ctx* ctx, void*, std::int32_t, const char*,
                       const dftu_open_how*, std::int32_t*, ::dftu_task**) {
    return io_unsupported(ctx, "openat2");
}

dftu_status io_getxattr(::dftu_ctx* ctx, void*, const char*, const char*, void*,
                        std::uint64_t, std::int64_t*, ::dftu_task**) {
    return io_unsupported(ctx, "getxattr");
}

dftu_status io_setxattr(::dftu_ctx* ctx, void*, const char*, const char*,
                        const void*, std::uint64_t, std::int32_t,
                        ::dftu_task**) {
    return io_unsupported(ctx, "setxattr");
}

dftu_status io_fgetxattr(::dftu_ctx* ctx, void*, std::int32_t, const char*,
                         void*, std::uint64_t, std::int64_t*, ::dftu_task**) {
    return io_unsupported(ctx, "fgetxattr");
}

dftu_status io_fsetxattr(::dftu_ctx* ctx, void*, std::int32_t, const char*,
                         const void*, std::uint64_t, std::int32_t,
                         ::dftu_task**) {
    return io_unsupported(ctx, "fsetxattr");
}

dftu_status io_tee(::dftu_ctx* ctx, void*, std::int32_t, std::int32_t,
                   std::uint64_t, std::uint32_t, std::int64_t*, ::dftu_task**) {
    return io_unsupported(ctx, "tee");
}

dftu_status io_pipe(::dftu_ctx* ctx, void*, std::int32_t[2], std::int32_t,
                    ::dftu_task**) {
    return io_unsupported(ctx, "pipe");
}

#endif

const dftu_io g_io = {sizeof(dftu_io),
                      io_open,
                      io_close,
                      io_read,
                      io_write,
                      io_pread,
                      io_pwrite,
                      io_fsync,
                      io_ftruncate,
                      io_fstat,
                      io_readv,
                      io_writev,
                      io_preadv,
                      io_pwritev,
                      io_lseek,
                      io_sendfile,
                      io_accept,
                      io_recv,
                      io_send,
                      io_unlink,
                      io_rmdir,
                      io_rename,
                      io_mkdir,
                      io_fadvise,
                      io_linkat,
                      io_symlinkat,
                      io_socket,
                      io_connect,
                      io_bind,
                      io_listen,
                      io_shutdown,
                      io_sendmsg,
                      io_recvmsg,
                      io_poll_add,
                      io_statx,
                      io_fallocate,
                      io_sync_file_range,
                      io_splice,
                      io_openat2,
                      io_getxattr,
                      io_setxattr,
                      io_fgetxattr,
                      io_fsetxattr,
                      io_tee,
                      io_pipe};

// DDSketch lacks a mean, so the box tracks the running weighted sum.
struct SketchBox : dstd::HeapObject {
    dataframe::DDSketch sketch;
    double sum = 0.0;
};

::dftu_sketch* host_sketch_create(void*) {
    return reinterpret_cast<::dftu_sketch*>(new (std::nothrow) SketchBox());
}

void host_sketch_add(void*, ::dftu_sketch* s, double v, double w) {
    if (!s) return;
    auto* b = reinterpret_cast<SketchBox*>(s);
    b->sketch.add(v, w);
    b->sum += v * w;
}

void host_sketch_merge(void*, ::dftu_sketch* into, const ::dftu_sketch* other) {
    if (!into || !other) return;
    auto* a = reinterpret_cast<SketchBox*>(into);
    const auto* b = reinterpret_cast<const SketchBox*>(other);
    a->sketch.merge(b->sketch);
    a->sum += b->sum;
}

::dftu_quantiles host_sketch_result(void*, const ::dftu_sketch* s) {
    ::dftu_quantiles q{};
    if (!s) return q;
    const auto* b = reinterpret_cast<const SketchBox*>(s);
    q.count = b->sketch.count();
    q.min = b->sketch.min();
    q.max = b->sketch.max();
    q.mean = q.count ? b->sum / static_cast<double>(q.count) : 0.0;
    q.p50 = b->sketch.quantile(0.5);
    q.p90 = b->sketch.quantile(0.9);
    q.p95 = b->sketch.quantile(0.95);
    q.p99 = b->sketch.quantile(0.99);
    return q;
}

void host_sketch_free(void*, ::dftu_sketch* s) {
    delete reinterpret_cast<SketchBox*>(s);
}

void append_json_escaped(dstd::String& out, dstd::StringView s) {
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += dstd::fmt<"\\u{:04x}">(static_cast<unsigned>(c));
                } else {
                    out += c;
                }
        }
    }
}

// Named-column access to a batch's dyn arg columns, resolved once per
// dftu_dataframe (not per row): each ("args.<key>", column) pair the frame
// carries. Used only for serializing a whole row's args (trace_write), which
// needs every dyn column, not a lookup by name (see utils::plugins::Batch's
// per-batch column cache for the per-row-loop counterpart).
struct DynCols {
    dstd::Vector<dstd::Pair<dstd::StringView, dftu_series*>> cols;

    explicit DynCols(const dftu_dataframe* df) {
        static constexpr dstd::StringView ARGS_PREFIX = "args.";
        const std::int32_t n = dftu_dataframe_num_columns(df);
        for (std::int32_t i = 0; i < n; ++i) {
            const char* name = dftu_dataframe_column_name(df, i);
            if (!name) continue;
            dstd::StringView nv{name};
            if (nv.substr(0, ARGS_PREFIX.size()) != ARGS_PREFIX) continue;
            cols.emplace_back(nv.substr(ARGS_PREFIX.size()),
                              dftu_dataframe_column(df, name));
        }
    }
    ~DynCols() {
        for (auto& [name, c] : cols) dftu_series_free(c);
    }
    DynCols(const DynCols&) = delete;
    DynCols& operator=(const DynCols&) = delete;
};

void serialize_row(dstd::String& out, const dftu_dataframe* df,
                   const DynCols& dyn, std::int64_t row, std::uint64_t id) {
    auto str_col = [&](const char* name, dstd::StringView* out_sv) {
        dftu_series* c = dftu_dataframe_column(df, name);
        if (!c) return;
        if (dftu_series_type(c) == DFTU_TYPE_STRING) {
            std::int64_t n = 0;
            if (const char* p = dftu_series_string_at(c, row, &n))
                *out_sv = {p, static_cast<std::size_t>(n)};
        }
        dftu_series_free(c);
    };
    auto u64_col = [&](const char* name) -> std::uint64_t {
        dftu_series* c = dftu_dataframe_column(df, name);
        if (!c) return 0;
        std::uint64_t v = 0;
        if (dftu_series_type(c) == DFTU_TYPE_UINT64 &&
            !dftu_series_is_null(c, row))
            v = static_cast<const std::uint64_t*>(dftu_series_data(c))[row];
        dftu_series_free(c);
        return v;
    };
    auto i64_col = [&](const char* name, bool* present) -> std::int64_t {
        dftu_series* c = dftu_dataframe_column(df, name);
        if (!c) return 0;
        std::int64_t v = 0;
        if (dftu_series_type(c) == DFTU_TYPE_INT64 &&
            !dftu_series_is_null(c, row)) {
            v = static_cast<const std::int64_t*>(dftu_series_data(c))[row];
            if (present) *present = true;
        }
        dftu_series_free(c);
        return v;
    };

    dstd::StringView name_v, cat_v;
    str_col("name", &name_v);
    str_col("cat", &cat_v);
    bool has_ph = false;
    const std::int64_t ph = i64_col("ph", &has_ph);
    const std::uint64_t pid = u64_col("pid");
    const std::uint64_t tid = u64_col("tid");
    const std::uint64_t ts = u64_col("ts");
    const std::uint64_t dur = u64_col("dur");

    out += "{\"id\":";
    out += dstd::to_string(id);
    out += ",\"name\":\"";
    append_json_escaped(out, name_v);
    out += "\",\"cat\":\"";
    append_json_escaped(out, cat_v);
    out += "\",\"ph\":";
    out += dstd::to_string(has_ph ? ph
                                  : static_cast<std::int64_t>(DFTU_PH_UNKNOWN));
    out += ",\"pid\":";
    out += dstd::to_string(pid);
    out += ",\"tid\":";
    out += dstd::to_string(tid);
    out += ",\"ts\":";
    out += dstd::to_string(ts);
    if (has_ph && ph == DFTU_PH_COMPLETE) {
        out += ",\"dur\":";
        out += dstd::to_string(dur);
    }
    bool args_open = false;
    for (const auto& [key, c] : dyn.cols) {
        if (dftu_series_is_null(c, row)) continue;
        if (!args_open) {
            out += ",\"args\":{";
            args_open = true;
        } else {
            out += ',';
        }
        out += '"';
        append_json_escaped(out, key);
        out += "\":";
        switch (dftu_series_type(c)) {
            case DFTU_TYPE_STRING: {
                std::int64_t n = 0;
                const char* p = dftu_series_string_at(c, row, &n);
                const dstd::StringView sv =
                    p ? dstd::StringView{p, static_cast<std::size_t>(n)}
                      : dstd::StringView{};
                out += '"';
                append_json_escaped(out, sv);
                out += '"';
                break;
            }
            case DFTU_TYPE_INT64:
                out += dstd::to_string(
                    static_cast<const std::int64_t*>(dftu_series_data(c))[row]);
                break;
            case DFTU_TYPE_FLOAT64: {
                char b[32];
                char* end = dstd::to_chars_general(
                    b, b + sizeof(b),
                    static_cast<const double*>(dftu_series_data(c))[row], 17);
                out.append(b, end);
                break;
            }
            default:
                out += "null";
                break;
        }
    }
    if (args_open) out += '}';
    out += "}\n";
}

dftu_status host_arrow_write_ipc(::dftu_ctx* ctx, void*,
                                 [[maybe_unused]] ::ArrowArray* a,
                                 [[maybe_unused]] ::ArrowSchema* s,
                                 [[maybe_unused]] const char* path) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (!a || !s || !path || !a->release || !s->release) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "arrow_write_ipc: the array, the schema and the path "
                         "must be set and not released");
    }
    nanoarrow::UniqueSchema schema;
    ArrowSchemaMove(s, schema.get());
    nanoarrow::UniqueArray array;
    ArrowArrayMove(a, array.get());
    trace::arrow::ArrowExportResult res(std::move(schema), std::move(array));
    const char* failed = nullptr;
    const auto ran = dftracer::utils::default_runtime().block_on(
        coro::named("dft-plugin-ipc", [&]() -> coro::CoroTask<void> {
            trace::arrow::IpcWriter writer;
            const auto open_rc = co_await writer.open(path);
            if (open_rc != 0) {
                failed = "open";
                co_return;
            }
            const auto write_rc = co_await writer.write_batch(res);
            if (write_rc != 0) {
                failed = "write";
                co_return;
            }
            const auto close_rc = co_await writer.close();
            if (close_rc != 0) failed = "close";
        }()));
    if (!ran) return host_fail(ctx, ran.error());
    ArrowSchemaMove(res.schema(), s);
    ArrowArrayMove(res.array(), a);
    if (failed != nullptr) {
        return host_fail(ctx, DFTU_E_IO,
                         dstd::fmt<"arrow_write_ipc: {} of '{}' failed">(
                             dstd::StringView(failed), dstd::StringView(path)));
    }
    return DFTU_OK;
#else
    return host_fail(ctx, DFTU_E_UNSUPPORTED,
                     "arrow_write_ipc: this build has no Arrow IPC support");
#endif
}

// Read the first record batch of an IPC file into out/out_schema; the caller
// must release both.
dftu_status host_arrow_read_ipc(::dftu_ctx* ctx, void*,
                                [[maybe_unused]] const char* path,
                                [[maybe_unused]] ::ArrowArray* out,
                                [[maybe_unused]] ::ArrowSchema* out_schema) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (!path || !out || !out_schema) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "arrow_read_ipc: the path and both outputs must be "
                         "set");
    }
    trace::arrow::IpcReader reader;
    if (reader.open(path) != 0) {
        return host_fail(ctx, DFTU_E_IO,
                         dstd::fmt<"arrow_read_ipc: cannot open '{}'">(
                             dstd::StringView(path)));
    }
    if (reader.batch_count() == 0) {
        return host_fail(ctx, DFTU_E_PARSE,
                         dstd::fmt<"arrow_read_ipc: '{}' has no record batch">(
                             dstd::StringView(path)));
    }
    trace::arrow::ArrowExportResult res = reader.read_batch(0);
    if (!res.array() || !res.schema() || !res.array()->release ||
        !res.schema()->release) {
        return host_fail(ctx, DFTU_E_PARSE,
                         dstd::fmt<"arrow_read_ipc: cannot read the first "
                                   "batch of '{}'">(dstd::StringView(path)));
    }
    ArrowArrayMove(res.array(), out);
    ArrowSchemaMove(res.schema(), out_schema);
    return DFTU_OK;
#else
    return host_fail(ctx, DFTU_E_UNSUPPORTED,
                     "arrow_read_ipc: this build has no Arrow IPC support");
#endif
}

dftu_status host_trace_open_write(::dftu_ctx* ctx, void* h, const char* path,
                                  ::dftu_trace_writer** out) {
    if (!path || !out) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "trace_open_write: the path and out must be set");
    }
    *out = static_cast<PluginFold*>(h)->make_trace_writer(path);
    return DFTU_OK;
}

dftu_status host_trace_write(::dftu_ctx* ctx, void*, ::dftu_trace_writer* w,
                             const dftu_dataframe* df) {
    if (!w || !df) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "trace_write: the writer and the frame must be set");
    }
    auto* tw = reinterpret_cast<PluginTraceWriter*>(w);
    DynCols dyn(df);
    const std::int64_t n = dftu_dataframe_num_rows(df);
    for (std::int64_t i = 0; i < n; ++i)
        serialize_row(tw->buffer, df, dyn, i, tw->next_id++);
    return DFTU_OK;
}

dftu_status host_trace_close(::dftu_ctx* ctx, void* h, ::dftu_trace_writer* w) {
    const dstd::Result<void> closed =
        static_cast<PluginFold*>(h)->close_trace_writer(w);
    if (!closed) return host_fail(ctx, closed.error());
    return DFTU_OK;
}

const dftu_svc_sketch g_sketch = {sizeof(dftu_svc_sketch), host_sketch_create,
                                  host_sketch_add,         host_sketch_merge,
                                  host_sketch_result,      host_sketch_free};

const dftu_svc_arrow g_arrow = {sizeof(dftu_svc_arrow), host_arrow_write_ipc,
                                host_arrow_read_ipc};

dftu_status host_trace_read(::dftu_ctx* ctx, void* h, const char* path,
                            dftu_stream_item_fn on_batch, void* ud) {
    if (!path || !on_batch) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "trace_read: the path and on_batch must be set");
    }
    auto& intern = static_cast<PluginFold*>(h)->intern_table();
    dstd::String p(path);
    dftu_status failed = DFTU_OK;
    const auto ran = dftracer::utils::default_runtime().block_on(coro::named(
        "dft-plugin-trace-read", [&]() -> coro::CoroTask<dstd::Result<void>> {
            dftracer::utils::json::Document document;
            dftracer::utils::dstd::InternCache cache(intern);
            trace::views::detail::FoldEventBuffer batch_events;
            auto detected = trace::index::detect_file_schema(p);
            if (!detected) co_return detected.error();
            const trace::index::RecordSchema& schema = **detected;
            const bool by_path = schema.decoder == trace::index::Decoder::Path;
            const trace::index::RecordSchema* declared =
                schema.fields.empty() ? nullptr : &schema;
            trace::views::View v = trace::views::View::from_file(p);
            auto scanned = co_await v.for_each_batch(
                [&](std::size_t, const dstd::Vector<dstd::StringView>& events) {
                    if (failed != DFTU_OK) return;
                    batch_events.clear();
                    for (dstd::StringView ev : events) {
                        auto parsed = document.parse(ev);
                        if (!parsed) continue;
                        json::Value& record = *parsed;
                        trace::views::detail::FoldEvent& fe =
                            batch_events.append();
                        if (by_path)
                            trace::views::detail::decode_record_into(
                                fe, record, intern,
                                /*capture_schema=*/false, nullptr, declared,
                                nullptr, 0, nullptr, false, &cache);
                        else
                            trace::views::detail::extract_fold_event_into(
                                fe, record, intern, /*needs_args=*/true,
                                &cache);
                        if (json::detail::ValueAccess::broken(record))
                            batch_events.discard_last();
                    }
                    if (batch_events.empty()) return;
                    dataframe::DataFrame df =
                        trace::views::detail::build_row_frame(
                            batch_events.events(), intern, {}, 1.0, by_path);
                    dstd::Vector<dftu_series*> handles;
                    handles.reserve(df.columns.size());
                    dstd::Vector<const char*> names;
                    names.reserve(df.names.size());
                    for (const auto& c : df.columns)
                        handles.push_back(dftu_series_share(c.handle()));
                    for (const auto& n : df.names) names.push_back(n.c_str());
                    dftu_dataframe* cdf = dftu_dataframe_new(
                        names.data(), handles.data(),
                        static_cast<std::int32_t>(handles.size()));
                    failed = on_batch(ctx, cdf, ud);
                    dftu_dataframe_free(cdf);
                },
                /*slot_count=*/1, /*limit=*/0);
            if (!scanned) co_return scanned.error();
            co_return dstd::Result<void>{};
        }()));
    if (!ran) return host_fail(ctx, ran.error());
    return failed;
}

const dftu_svc_trace g_trace = {sizeof(dftu_svc_trace), host_trace_open_write,
                                host_trace_write, host_trace_close,
                                host_trace_read};

dftu_status host_query_compile(::dftu_ctx* ctx, void* h, const char* src,
                               std::uint32_t len, dftu_duql** out) {
    if (!src || !out) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "duql_compile: the source and out must be set");
    }
    auto compiled = static_cast<PluginFold*>(h)->compile_query(src, len);
    if (!compiled) {
        return host_fail(ctx, DFTU_E_PARSE, compiled.error().message());
    }
    *out = *compiled;
    return DFTU_OK;
}

std::int32_t host_query_matches(void* h, const dftu_duql* q,
                                const dftu_dataframe* df, std::int64_t row) {
    if (!q || !df) return 0;
    return static_cast<PluginFold*>(h)->match_query(
        *reinterpret_cast<const Query*>(q), df, row);
}

std::uint64_t host_port_key(void*, const char* cap_id) {
    return cap_id ? dftracer::utils::dstd::hash::fnv1a_hash(cap_id) : 0;
}

void host_port_publish(void* h, std::uint64_t key, const void* data,
                       std::uint32_t len) {
    static_cast<PluginFold*>(h)->port_publish(key, data, len);
}

const void* host_port_consume(void* h, std::uint64_t key,
                              std::uint32_t* out_size) {
    return static_cast<PluginFold*>(h)->port_consume(key, out_size);
}

const dftu_svc_ports g_ports = {sizeof(dftu_svc_ports), host_port_key,
                                host_port_publish, host_port_consume};

const dftu_svc_duql g_query = {sizeof(dftu_svc_duql), host_query_compile,
                               host_query_matches};

// Stateless ext tables share one instance across all folds (each fn takes h).
const void* host_get_service(void*, const char* ext_id) {
    if (!ext_id) return nullptr;
    if (std::strcmp(ext_id, DFTU_SVC_IO) == 0) return &g_io;
    if (std::strcmp(ext_id, DFTU_SVC_TASK) == 0)
        return detail::task_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_DUQL) == 0) return &g_query;
    if (std::strcmp(ext_id, DFTU_SVC_FILEIO) == 0)
        return detail::fileio_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_SKETCH) == 0) return &g_sketch;
    if (std::strcmp(ext_id, DFTU_SVC_ARROW) == 0) return &g_arrow;
    if (std::strcmp(ext_id, DFTU_SVC_TRACE) == 0) return &g_trace;
    if (std::strcmp(ext_id, DFTU_SVC_PORTS) == 0) return &g_ports;
    if (std::strcmp(ext_id, DFTU_SVC_RESULT) == 0)
        return detail::result_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_AGG) == 0) return detail::agg_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_OPS) == 0) return detail::ops_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_PROVIDERS) == 0)
        return detail::providers_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_NODES) == 0)
        return detail::nodes_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_INDEX) == 0)
        return detail::index_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_FS) == 0) return detail::fs_ext_vtable();
    return nullptr;
}

}  // namespace

PluginHandle* PluginFold::adopt_handle(dstd::UniquePtr<PluginHandle> handle) {
    std::lock_guard<std::mutex> lock(handles_mutex_);
    handles_.push_back(std::move(handle));
    return handles_.back().get();
}

void PluginFold::release_handle(const void* raw) {
    dstd::UniquePtr<PluginHandle> released;
    {
        std::lock_guard<std::mutex> lock(handles_mutex_);
        for (auto it = handles_.begin(); it != handles_.end(); ++it) {
            if (static_cast<const void*>(it->get()) == raw) {
                released = std::move(*it);
                handles_.erase(it);
                break;
            }
        }
    }
}

::dftu_trace_writer* PluginFold::make_trace_writer(const char* path) {
    if (!path) return nullptr;
    auto tw = dstd::make_unique<PluginTraceWriter>();
    tw->path = path;
    trace_writers_.push_back(std::move(tw));
    return reinterpret_cast<::dftu_trace_writer*>(trace_writers_.back().get());
}

namespace {
// Compress the buffered NDJSON into whole-line gzip members and write a
// re-indexable .pfw.gz. The index is built lazily on first read, not here.
dstd::Result<void> write_trace_file(const PluginTraceWriter& tw) {
    fileio::GzipWriterOptions opts;
    opts.member_size = common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    const std::size_t member_size = opts.member_size;
    auto opened =
        fileio::GzipLineWriterBlocking::open(tw.path, std::move(opts));
    if (!opened) {
        return common::make_error<"trace_close: cannot open '{}': {}">(
            common::ErrorCode::Io, tw.path, opened.error().message());
    }
    auto& writer = *opened;
    const dstd::StringView buf = tw.buffer;
    const std::size_t end = buf.rfind('\n') + 1;
    std::size_t pos = 0;
    while (pos < end) {
        std::size_t cut = buf.find('\n', pos + member_size);
        cut = cut == dstd::StringView::npos || cut >= end ? end : cut + 1;
        if (!writer.append(buf.substr(pos, cut - pos))) {
            return common::make_error<"trace_close: cannot write '{}'">(
                common::ErrorCode::Io, tw.path);
        }
        pos = cut;
    }
    if (!writer.close(buf.substr(end))) {
        return common::make_error<"trace_close: cannot finish '{}'">(
            common::ErrorCode::Io, tw.path);
    }
    return {};
}
}  // namespace

dstd::Result<void> PluginFold::close_trace_writer(::dftu_trace_writer* w) {
    auto* tw = reinterpret_cast<PluginTraceWriter*>(w);
    if (!tw) {
        return common::make_error<"trace_close: the writer is NULL">(
            common::ErrorCode::InvalidArgument);
    }
    dstd::Result<void> result = write_trace_file(*tw);
    for (auto it = trace_writers_.begin(); it != trace_writers_.end(); ++it) {
        if (it->get() == tw) {
            trace_writers_.erase(it);
            break;
        }
    }
    return result;
}

PluginFold::PluginFold(const dftu_plugin* plugin,
                       dftracer::utils::dstd::StringIntern& intern,
                       SharedResultRegistry* results,
                       utils::plugins::NamedResultRegistry* named_results,
                       dstd::String plugin_name, std::uint64_t memory_budget,
                       const StateRegistry* states)
    : plugin_(plugin),
      plugin_name_(std::move(plugin_name)),
      intern_(&intern),
      results_(results),
      named_results_(named_results),
      memory_budget_(memory_budget),
      states_(states) {
    if (states_) {
        state_accums_.reserve(states_->size());
        for (const RegisteredState& reg : *states_)
            state_accums_.push_back(dstd::make_unique<StateAccum>(
                reg, memory_budget_, plugin_label()));
    }

    if (plugin_->make_slice) {
        plugins::AbiContext ctx;
        void* made = nullptr;
        const dftu_status status =
            plugin_->make_slice(ctx.get(), plugin_->self, &made);
        if (status < 0) {
            slice_error_ = callback_error(plugin_label(), "make_slice", status,
                                          ctx.message());
        } else {
            slice_ = made;
        }
    }

    host_.struct_size = sizeof(dftu_plugin_host);
    host_.h = this;
    host_.get_service = host_get_service;
    host_.resolve = host_resolve;
    host_.intern = host_intern;
    host_.log = host_log;

    // An invalid filter is logged and left unset rather than aborting.
    const char* q =
        plugin_->plan_duql ? plugin_->plan_duql(plugin_->self) : nullptr;
    if (q && *q) {
        auto r = Query::from_string(q);
        if (r)
            query_ = std::move(*r);
        else
            DFTRACER_UTILS_LOG_ERROR("Plugin query '{}' is invalid: {}", q,
                                     r.error().message);
    }

    // Registered states are handed the SAME frame as the slice, and what they
    // read is invisible from the slice that declared the projection. A state
    // whose column was projected away would just see a NULL lookup and answer
    // wrongly, so a plugin with states keeps every column: a projection is an
    // optimisation, and losing it must never cost correctness.
    if (plugin_->reads && state_accums_.empty()) {
        const char* const* cols = plugin_->reads(plugin_->self);
        for (; cols && *cols; ++cols) projection_.emplace_back(*cols);
    }
}

PluginFold::~PluginFold() {
    if (slice_) plugin_->destroy_slice(slice_);
}

dstd::Result<::dftu_duql*> PluginFold::compile_query(const char* src,
                                                     std::uint32_t len) {
    if (!src) {
        return common::make_error<
            "plugin '{}' duql_compile: the source is "
            "NULL">(common::ErrorCode::InvalidArgument, plugin_label());
    }
    auto r = Query::from_string(dstd::StringView{src, len});
    if (!r)
        return common::make_error(common::ErrorCode::Duql, r.error().message);
    compiled_queries_.push_back(std::move(*r));
    return reinterpret_cast<::dftu_duql*>(&compiled_queries_.back());
}

const PluginFold::MatchColumns& PluginFold::match_columns(
    const Query& q, const dftu_dataframe* df) {
    const std::uint64_t serial = dataframe::dataframe_handle_serial(df);
    if (serial != match_frame_serial_) {
        match_columns_.clear();
        match_frame_serial_ = serial;
    }
    for (const MatchColumns& m : match_columns_)
        if (m.query == &q) return m;

    const dataframe::DataFrame& frame = dataframe::dataframe_handle_view(df);
    MatchColumns m{&q, frame.num_rows(), {}};
    dstd::String args_name;
    for (dstd::StringView f : q.fields()) {
        // A path-decoded frame names each column by its path.
        dataframe::Series c = frame.column(f);
        const bool top = f == "cat" || f == "name" || f == "fhash" ||
                         f == "hhash" || f == "pid" || f == "tid" ||
                         f == "ts" || f == "dur";
        if (!c.valid() && !top) {
            args_name.assign("args.");
            args_name.append(common::strip_args_prefix(f));
            c = frame.column(args_name);
        }
        if (!c.valid()) continue;
        // Numbers are read from the value buffer, which only a FLAT column
        // has; strings read every encoding through string_at.
        if (!c.is_flat() && c.type() != dataframe::TypeId::String)
            c = c.materialize();
        m.columns.emplace_back(f, std::move(c));
    }
    match_columns_.push_back(std::move(m));
    return match_columns_.back();
}

int PluginFold::match_query(const Query& q, const dftu_dataframe* df,
                            std::int64_t row) {
    const MatchColumns& m = match_columns(q, df);
    if (row < 0 || row >= m.rows) return 0;
    match_qmap_.clear();
    for (const auto& [f, c] : m.columns) {
        if (c.is_null(row)) continue;
        switch (c.type()) {
            case dataframe::TypeId::Uint64:
                match_qmap_[f] = c.data<std::uint64_t>()[row];
                break;
            case dataframe::TypeId::Int64:
                match_qmap_[f] = c.data<std::int64_t>()[row];
                break;
            case dataframe::TypeId::Float64:
                match_qmap_[f] = c.data<double>()[row];
                break;
            case dataframe::TypeId::String:
                match_qmap_[f] = dstd::String(c.string_at(row));
                break;
            default:
                break;
        }
    }
    return q.evaluate(match_qmap_) ? 1 : 0;
}

// The same field reads as the scan's pod_matches, so plan_duql and
// duql_matches agree with it.
bool PluginFold::passes_query(const FoldEvent& ev) {
    return trace::views::detail::pod_matches(*query_, ev, *intern_, qmap_);
}

// Materialize the (query-passing, non-metadata) events of this batch into a
// native DataFrame and hand it across the ABI as a dftu_dataframe, so the
// plugin runs SIMD column ops instead of a per-event loop. build_row_frame is
// the same columnar materializer NativeRowFold uses.
dstd::Result<void> PluginFold::step(const FoldBatch& batch) {
    if (slice_error_) return *slice_error_;
    // A transform steps by rewriting the batch; on_batch is not called
    // beside it (the transform accumulates inside its own call).
    const bool has_transform = slice_ && plugin_->transform;
    const bool has_on_batch = slice_ && plugin_->on_batch && !has_transform;
    if (!has_on_batch && !has_transform && state_accums_.empty()) return {};

    // A transform sees the whole batch, whatever it declared in reads.
    const dstd::Vector<dstd::String> every_column;
    const dstd::Vector<dstd::String>& projection =
        has_transform ? every_column : projection_;

    dataframe::DataFrame df;
    if (port_bus_ && port_bus_->frame) {
        df = rewritten_batch(*port_bus_->frame, projection);
    } else {
        col_scratch_.clear();
        for (const auto& fe : batch.events) {
            if (fe.phase == trace::RecordPhase::Metadata ||
                fe.phase == trace::RecordPhase::Unknown)
                continue;
            if (query_ && !passes_query(fe)) continue;
            col_scratch_.push_back(fe);
        }
        if (col_scratch_.empty()) return {};
        // An empty projection is build_row_frame's "every column", which for
        // a batch with many arg keys is a Series per key.
        df = trace::views::detail::build_row_frame(
            col_scratch_, *intern_, projection, 1.0,
            col_scratch_.front().by_path);
    }
    if (df.num_rows() == 0) return {};

    // dftu_dataframe_new takes ownership of the column handles, so pass shared
    // copies (a refcount bump, no data copy); df keeps its own.
    dstd::Vector<dftu_series*> handles;
    handles.reserve(df.columns.size());
    dstd::Vector<const char*> names;
    names.reserve(df.names.size());
    for (const auto& c : df.columns)
        handles.push_back(dftu_series_share(c.handle()));
    for (const auto& n : df.names) names.push_back(n.c_str());

    dftu_dataframe* cdf =
        dftu_dataframe_new(names.data(), handles.data(),
                           static_cast<std::int32_t>(handles.size()));
    dstd::Result<void> stepped = states_update(cdf);
    if (stepped && has_transform) {
        plugins::AbiContext ctx;
        ::dftu_dataframe* out = nullptr;
        const dftu_status status =
            plugin_->transform(ctx.get(), slice_, cdf, &host_, &out);
        if (status < 0) {
            stepped = callback_error(plugin_label(), "transform", status,
                                     ctx.message());
            if (out) dftu_dataframe_free(out);
        } else if (out) {
            // Every fold after this one in the batch reads the rewrite; with
            // no bus (a fold driven alone) there is no one to hand it to.
            if (port_bus_)
                port_bus_->frame = dataframe::dataframe_handle_take(out);
            else
                dftu_dataframe_free(out);
        }
    } else if (stepped && has_on_batch) {
        plugins::AbiContext ctx;
        const dftu_status status =
            plugin_->on_batch(ctx.get(), slice_, cdf, &host_);
        if (status < 0) {
            stepped = callback_error(plugin_label(), "on_batch", status,
                                     ctx.message());
        }
    }
    dftu_dataframe_free(cdf);
    if (!stepped) return stepped;
    reclaim_slice();
    return {};
}

// The batch a transform earlier in this step rewrote, cut down to this
// plugin's projection (by name; a column the rewrite dropped is absent, as
// reads documents) and to its plan_duql, which the events path applied per
// event and here runs as a frame mask. A query the frame cannot evaluate
// (a field the rewrite removed, a pattern match) is reported once and the
// rows pass unfiltered rather than being dropped.
dataframe::DataFrame PluginFold::rewritten_batch(
    const dataframe::DataFrame& frame,
    const dstd::Vector<dstd::String>& projection) {
    dataframe::DataFrame df;
    if (projection.empty()) {
        df.names = frame.names;
        for (const dataframe::Series& c : frame.columns)
            df.columns.push_back(c.share());
    } else {
        for (const dstd::String& name : projection) {
            for (std::size_t i = 0; i < frame.names.size(); ++i) {
                if (dstd::StringView(frame.names[i]) != name) continue;
                df.names.emplace_back(name);
                df.columns.push_back(frame.columns[i].share());
                break;
            }
        }
    }
    if (!query_ || df.columns.empty()) return df;
    auto mask = df.mask(*query_);
    if (mask) return df.filter(*mask);
    if (!query_on_rewrite_refused_) {
        query_on_rewrite_refused_ = true;
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:{}] its query cannot be applied to a batch an earlier "
            "plugin rewrote ({}); the rewritten rows pass unfiltered",
            plugin_name().empty() ? "plugin" : plugin_name(),
            mask.error().message());
    }
    return df;
}

void PluginFold::reclaim_slice() {
    if (!slice_ || !plugin_->bytes) return;
    const std::uint64_t budget =
        dftracer::utils::memory::resolve_spill_budget(memory_budget_);
    if (budget == dftracer::utils::memory::NO_SPILL_BUDGET) return;
    const std::uint64_t held = plugin_->bytes(slice_);
    if (held <= budget) return;
    const std::uint64_t freed =
        plugin_->reclaim ? plugin_->reclaim(slice_, held - budget) : 0;
    ++slice_reclaims_;
    if (freed == 0 && !reclaim_refused_) {
        reclaim_refused_ = true;
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:{}] slice holds {} bytes past the {} byte memory budget "
            "and {}, so it stays in memory and the budget is not honoured",
            plugin_name().empty() ? "plugin" : plugin_name(),
            static_cast<unsigned long long>(held),
            static_cast<unsigned long long>(budget),
            plugin_->reclaim ? "reclaim freed nothing" : "declares no reclaim");
    }
}

dstd::Result<void> PluginFold::merge(Fold& other) {
    auto& o = static_cast<PluginFold&>(other);
    if (auto states = states_merge(o); !states) return states.error();
    if (!slice_) return {};
    if (o.slice_) {
        plugins::AbiContext ctx;
        const dftu_status status = plugin_->merge(ctx.get(), slice_, o.slice_);
        if (status < 0)
            return callback_error(plugin_label(), "merge", status,
                                  ctx.message());
    }
    slice_reclaims_ += o.slice_reclaims_;
    reclaim_refused_ = reclaim_refused_ || o.reclaim_refused_;
    reclaim_slice();

    // Fold each worker's aggregation accumulators into this master by name via
    // the engine's single merge path (agg_merge). An accumulator new to the
    // master is moved in whole; a shared name merges the two AggStates.
    for (const auto& [key, idx] : o.aggs_.index()) {
        dstd::UniquePtr<AggAccum>& src = o.aggs_[idx];
        if (!src || !src->state) continue;
        dstd::UniquePtr<AggAccum>* dst = aggs_.find(key);
        if (!dst) {
            aggs_.push(key, std::move(src));
        } else if ((*dst)->state) {
            dataframe::agg_merge(*(*dst)->state, *src->state);
            // The worker's spilled runs are part of its aggregate, so the
            // master takes them over with the in-memory half.
            (*dst)->spiller.adopt(src->spiller);
            if (auto spilled = (*dst)->spiller.maybe_spill((*dst)->state);
                !spilled.has_value())
                return spilled.error();
        }
    }
    return {};
}

coro::CoroTask<dstd::Result<bool>> PluginFold::finalize(const CoverageSet&) {
    if (slice_error_) co_return *slice_error_;
    // Publish before on_finalize so a plugin finalizing later in fold order can
    // read this one's merged accumulators via agg_result().
    publish_aggs();
    auto materialized = materialize_aggs();
    if (!materialized) co_return materialized.error();
    auto states = states_finalize();
    if (!states) co_return states.error();
    if (slice_ && plugin_->on_finalize) {
        const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
        plugins::AbiContext ctx(cancel);
        ::dftu_task* pending = nullptr;
        const dftu_status status =
            plugin_->on_finalize(ctx.get(), slice_, &host_, &pending);
        if (status < 0) {
            if (pending) plugins::free_abi_task(pending);
            co_return callback_error(plugin_label(), "on_finalize", status,
                                     ctx.message());
        }
        if (pending && plugins::abi_task(pending)->awaited) {
            co_return callback_error(plugin_label(), "on_finalize",
                                     DFTU_E_INVALID,
                                     "pending is a task that was already "
                                     "awaited");
        }
        if (pending) {
            plugins::AbiTask* task = plugins::abi_task(pending);
            const dftu_status done = co_await plugins::run_abi_task(task);
            const dstd::Error failure =
                done < 0 ? plugins::abi_task_error(*task) : dstd::Error();
            plugins::free_abi_task(pending);
            if (done < 0) {
                co_return callback_error(plugin_label(), "on_finalize", done,
                                         failure.message());
            }
        }
    }
    co_return false;
}

}  // namespace dftracer::utils::plugins::host
