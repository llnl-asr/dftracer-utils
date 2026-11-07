// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_OPS_H
#define DFTRACER_UTILS_RUNTIME_IO_OPS_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/runtime/io/awaitable.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <csignal>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::runtime::io {

/// Asynchronous read (sequential). If inside an executor worker, submits
/// to the I/O backend and suspends. Otherwise falls back to blocking
/// read(). Works on any fd type (pipes, sockets, terminals, files).
IoAwaitable read(dstd::NativeHandle fd, void* buf, std::size_t len) noexcept;

/// Asynchronous write (sequential). Works on any fd type.
IoAwaitable write(dstd::NativeHandle fd, const void* buf,
                  std::size_t len) noexcept;

/// Asynchronous positional read. Only works on seekable fds (regular
/// files). Uses pread() under the hood.
IoAwaitable pread(dstd::NativeHandle fd, void* buf, std::size_t len,
                  std::uint64_t offset) noexcept;

/// Asynchronous positional write. Only works on seekable fds.
IoAwaitable pwrite(dstd::NativeHandle fd, const void* buf, std::size_t len,
                   std::uint64_t offset) noexcept;

/// Asynchronous open. Returns fd (>= 0) or negative errno.
IoAwaitable open(const char* path, int flags, mode_t mode = 0644) noexcept;

/// Asynchronous open of a file as a @ref dstd::File. The error has the shape
/// @ref dstd::File::open reports. @p path need not end with a NUL byte and
/// must stay valid until the task ends.
coro::CoroTask<dstd::Expected<dstd::File, dstd::Error>> open_file(
    dstd::PathView path, dstd::OpenOptions options);

/// Asynchronous close.
IoAwaitable close(dstd::NativeHandle fd) noexcept;

/// Asynchronous fsync.
IoAwaitable fsync(dstd::NativeHandle fd) noexcept;

/// Asynchronous ftruncate.
IoAwaitable ftruncate(dstd::NativeHandle fd, std::uint64_t length) noexcept;

/// Asynchronous fstat.
IoAwaitable fstat(dstd::NativeHandle fd, struct stat* buf) noexcept;

/// Asynchronous unlink of a file or symlink. @p path must stay valid until the
/// await ends.
IoAwaitable unlink(const char* path) noexcept;

/// Asynchronous rmdir. Gives -ENOTEMPTY for a directory that has entries.
/// @p path must stay valid until the await ends.
IoAwaitable rmdir(const char* path) noexcept;

/// Asynchronous rename. @p from and @p to must stay valid until the await ends.
IoAwaitable rename(const char* from, const char* to) noexcept;

/// Asynchronous mkdir. @p path must stay valid until the await ends.
IoAwaitable mkdir(const char* path, mode_t mode = 0777) noexcept;

/// Asynchronous posix_fadvise. Gives 0 where the platform has none.
IoAwaitable fadvise(dstd::NativeHandle fd, std::uint64_t offset,
                    std::uint64_t length, int advice) noexcept;

/// Asynchronous linkat. @p from and @p to must stay valid until the await ends.
IoAwaitable linkat(dstd::NativeHandle from_dirfd, const char* from,
                   dstd::NativeHandle to_dirfd, const char* to,
                   int flags) noexcept;

/// Asynchronous symlinkat. @p target and @p link must stay valid until the
/// await ends.
IoAwaitable symlinkat(const char* target, dstd::NativeHandle dirfd,
                      const char* link) noexcept;

/// Asynchronous socket. Gives the descriptor.
IoAwaitable socket(int domain, int type, int protocol) noexcept;

/// Asynchronous connect. @p addr must stay valid until the await ends.
IoAwaitable connect(dstd::NativeHandle fd, const struct sockaddr* addr,
                    socklen_t addrlen) noexcept;

/// Asynchronous bind. @p addr must stay valid until the await ends.
IoAwaitable bind(dstd::NativeHandle fd, const struct sockaddr* addr,
                 socklen_t addrlen) noexcept;

/// Asynchronous listen.
IoAwaitable listen(dstd::NativeHandle fd, int backlog) noexcept;

/// Asynchronous shutdown of one or both directions of a socket.
IoAwaitable shutdown(dstd::NativeHandle fd, int how) noexcept;

/// Asynchronous sendmsg. @p msg, and what it points at, must stay valid until
/// the await ends.
IoAwaitable sendmsg(dstd::NativeHandle fd, const struct msghdr* msg,
                    int flags = 0) noexcept;

/// Asynchronous recvmsg. @p msg, and what it points at, must stay valid until
/// the await ends.
IoAwaitable recvmsg(dstd::NativeHandle fd, struct msghdr* msg,
                    int flags = 0) noexcept;

/// Asynchronous madvise.
IoAwaitable madvise(void* addr, std::size_t length, int advice) noexcept;

/// Asynchronous waitid. Waits for a child as waitid(2) does, so @p options
/// needs a WEXITED, WSTOPPED or WCONTINUED. Gives 0 and fills @p info. @p info
/// must stay valid until the await ends. On the thread pool the wait holds a
/// thread, and @ref IoBackend::cancel_pending cannot end it.
IoAwaitable waitid(int idtype, id_t id, siginfo_t* info, int options) noexcept;

/// Asynchronous poll of one descriptor, without a timeout. Gives the events
/// that happened.
IoAwaitable poll_add(dstd::NativeHandle fd, unsigned events) noexcept;

#ifdef __linux__
/// Asynchronous statx. @p path and @p out must stay valid until the await
/// ends. @p dirfd is AT_FDCWD for a path that is not relative to a directory.
IoAwaitable statx(dstd::NativeHandle dirfd, const char* path, int flags,
                  std::uint32_t mask, Statx* out) noexcept;

/// Asynchronous fallocate.
IoAwaitable fallocate(dstd::NativeHandle fd, int mode, std::uint64_t offset,
                      std::uint64_t length) noexcept;

/// Asynchronous sync_file_range.
IoAwaitable sync_file_range(dstd::NativeHandle fd, std::uint64_t offset,
                            std::uint64_t length, unsigned flags) noexcept;

/// Asynchronous splice. One of @p in_fd and @p out_fd must be a pipe, whose
/// offset is -1. Another offset of -1 uses the file position. May move fewer
/// bytes than @p length, and waits for a peer when a pipe is empty or full.
IoAwaitable splice(dstd::NativeHandle in_fd, std::int64_t in_offset,
                   dstd::NativeHandle out_fd, std::int64_t out_offset,
                   std::size_t length, unsigned flags) noexcept;

/// Asynchronous openat2. Gives the descriptor. @p path and @p how must stay
/// valid until the await ends.
IoAwaitable openat2(dstd::NativeHandle dirfd, const char* path,
                    const OpenHow& how) noexcept;

/// Asynchronous getxattr. Gives the size of the value. @p path and @p args,
/// and what it names, must stay valid until the await ends.
IoAwaitable getxattr(const char* path, const XattrArgs& args) noexcept;

/// Asynchronous setxattr. @p path and @p args, and what it names, must stay
/// valid until the await ends.
IoAwaitable setxattr(const char* path, const XattrArgs& args) noexcept;

/// Asynchronous fgetxattr. @p args, and what it names, must stay valid until
/// the await ends.
IoAwaitable fgetxattr(dstd::NativeHandle fd, const XattrArgs& args) noexcept;

/// Asynchronous fsetxattr. @p args, and what it names, must stay valid until
/// the await ends.
IoAwaitable fsetxattr(dstd::NativeHandle fd, const XattrArgs& args) noexcept;

/// Asynchronous tee. Copies up to @p length bytes from one pipe to another and
/// leaves them in the first. Waits for a peer when @p in_fd is empty or
/// @p out_fd is full.
IoAwaitable tee(dstd::NativeHandle in_fd, dstd::NativeHandle out_fd,
                std::size_t length, unsigned flags) noexcept;

/// Asynchronous pipe2. Fills @p fds, which must stay valid until the await
/// ends.
IoAwaitable pipe(int* fds, int flags = 0) noexcept;

/// Asynchronous wait on a futex word, until a wake or until the word differs
/// from @p expected, which gives -EAGAIN. @p futex_flags are
/// @ref IO_FUTEX_SIZE_U32 with @ref IO_FUTEX_PRIVATE. @p uaddr must stay valid
/// until the await ends. On the thread pool the wait holds a thread, and
/// @ref IoBackend::cancel_pending cannot end it.
IoAwaitable futex_wait(const std::uint32_t* uaddr, std::uint32_t expected,
                       std::uint64_t mask = IO_FUTEX_MATCH_ANY,
                       unsigned futex_flags = IO_FUTEX_SIZE_U32 |
                                              IO_FUTEX_PRIVATE) noexcept;

/// Asynchronous wake of up to @p count waiters of a futex word. Gives how many
/// it woke.
IoAwaitable futex_wake(std::uint32_t* uaddr, std::uint32_t count,
                       std::uint64_t mask = IO_FUTEX_MATCH_ANY,
                       unsigned futex_flags = IO_FUTEX_SIZE_U32 |
                                              IO_FUTEX_PRIVATE) noexcept;

/// Asynchronous wait on several futexes. Gives the index of the one that woke.
/// @p waiters must stay valid until the await ends. On the thread pool the
/// wait holds a thread, and @ref IoBackend::cancel_pending cannot end it.
IoAwaitable futex_waitv(const FutexWaitv* waiters, unsigned count) noexcept;

/// Asynchronous epoll_ctl. @p event must stay valid until the await ends.
IoAwaitable epoll_ctl(dstd::NativeHandle epfd, int op, dstd::NativeHandle fd,
                      struct epoll_event* event) noexcept;

/// Asynchronous epoll_wait, without a timeout. Gives the event count. @p events
/// must stay valid until the await ends.
IoAwaitable epoll_wait(dstd::NativeHandle epfd, struct epoll_event* events,
                       int max_events) noexcept;
#endif

/// Runs @p fn with @p context off the worker and gives its result: on an I/O
/// thread, inline on a backend with none or outside an executor. For a blocking
/// call that has no op of its own, such as a directory listing. @p context must
/// stay valid until the await ends.
IoAwaitable call(BlockingFn fn, void* context) noexcept;

/// Accept a connection on a listening socket. Returns client fd.
IoAwaitable accept(dstd::NativeHandle listen_fd,
                   struct sockaddr* addr = nullptr,
                   socklen_t* addrlen = nullptr) noexcept;

/// Receive data from a connected socket. Returns bytes received.
IoAwaitable recv(dstd::NativeHandle fd, void* buf, std::size_t len,
                 int flags = 0) noexcept;

/// Send data to a connected socket. Returns bytes sent.
IoAwaitable send(dstd::NativeHandle fd, const void* buf, std::size_t len,
                 int flags = 0) noexcept;

/// Scatter-gather sequential read. Works on any fd type.
IoAwaitable readv(dstd::NativeHandle fd, const struct iovec* iov,
                  int iovcnt) noexcept;

/// Scatter-gather sequential write. Works on any fd type. Like writev(2), it
/// accepts at most IOV_MAX entries and may write fewer bytes than requested;
/// use writev_all() unless you handle both.
IoAwaitable writev(dstd::NativeHandle fd, const struct iovec* iov,
                   int iovcnt) noexcept;

/// writev() until every byte is sent: splits runs longer than IOV_MAX and
/// resumes after short writes. `iov` is mutated as entries are consumed.
/// Returns the bytes written, or a negative errno on failure.
coro::CoroTask<ssize_t> writev_all(dstd::NativeHandle fd, struct iovec* iov,
                                   int iovcnt);

/// Scatter-gather positional read. Only works on seekable fds.
IoAwaitable preadv(dstd::NativeHandle fd, const struct iovec* iov, int iovcnt,
                   std::uint64_t offset) noexcept;

/// Scatter-gather positional write. Only works on seekable fds.
IoAwaitable pwritev(dstd::NativeHandle fd, const struct iovec* iov, int iovcnt,
                    std::uint64_t offset) noexcept;

/// Reposition file offset. Returns new offset or negative errno.
IoAwaitable lseek(dstd::NativeHandle fd, std::int64_t offset,
                  int whence) noexcept;

/// Zero-copy transfer from in_fd to out_fd. Returns bytes sent.
/// in_fd must be a regular file. Starts at the given offset.
IoAwaitable sendfile(dstd::NativeHandle out_fd, dstd::NativeHandle in_fd,
                     std::uint64_t offset, std::size_t count) noexcept;

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_OPS_H
