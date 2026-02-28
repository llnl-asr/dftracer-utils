#ifndef DFTRACER_UTILS_CORE_IO_OPS_H
#define DFTRACER_UTILS_CORE_IO_OPS_H

#include <dftracer/utils/core/io/awaitable.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>

namespace dftracer::utils::io {

/// Asynchronous read (sequential). If inside an executor worker, submits
/// to the I/O backend and suspends. Otherwise falls back to blocking
/// read(). Works on any fd type (pipes, sockets, terminals, files).
IoAwaitable read(int fd, void* buf, std::size_t len) noexcept;

/// Asynchronous write (sequential). Works on any fd type.
IoAwaitable write(int fd, const void* buf, std::size_t len) noexcept;

/// Asynchronous positional read. Only works on seekable fds (regular
/// files). Uses pread() under the hood.
IoAwaitable pread(int fd, void* buf, std::size_t len, off_t offset) noexcept;

/// Asynchronous positional write. Only works on seekable fds.
IoAwaitable pwrite(int fd, const void* buf, std::size_t len,
                   off_t offset) noexcept;

/// Asynchronous open. Returns fd (>= 0) or negative errno.
IoAwaitable open(const char* path, int flags, mode_t mode = 0644) noexcept;

/// Asynchronous close.
IoAwaitable close(int fd) noexcept;

/// Asynchronous fsync.
IoAwaitable fsync(int fd) noexcept;

/// Asynchronous ftruncate.
IoAwaitable ftruncate(int fd, off_t length) noexcept;

/// Asynchronous fstat.
IoAwaitable fstat(int fd, struct stat* buf) noexcept;

/// Accept a connection on a listening socket. Returns client fd.
IoAwaitable accept(int listen_fd, struct sockaddr* addr = nullptr,
                   socklen_t* addrlen = nullptr) noexcept;

/// Receive data from a connected socket. Returns bytes received.
IoAwaitable recv(int fd, void* buf, std::size_t len, int flags = 0) noexcept;

/// Send data to a connected socket. Returns bytes sent.
IoAwaitable send(int fd, const void* buf, std::size_t len,
                 int flags = 0) noexcept;

/// Scatter-gather sequential read. Works on any fd type.
IoAwaitable readv(int fd, const struct iovec* iov, int iovcnt) noexcept;

/// Scatter-gather sequential write. Works on any fd type.
IoAwaitable writev(int fd, const struct iovec* iov, int iovcnt) noexcept;

/// Scatter-gather positional read. Only works on seekable fds.
IoAwaitable preadv(int fd, const struct iovec* iov, int iovcnt,
                   off_t offset) noexcept;

/// Scatter-gather positional write. Only works on seekable fds.
IoAwaitable pwritev(int fd, const struct iovec* iov, int iovcnt,
                    off_t offset) noexcept;

/// Reposition file offset. Returns new offset or negative errno.
IoAwaitable lseek(int fd, off_t offset, int whence) noexcept;

/// Zero-copy transfer from in_fd to out_fd. Returns bytes sent.
/// in_fd must be a regular file. Starts at the given offset.
IoAwaitable sendfile(int out_fd, int in_fd, off_t offset,
                     std::size_t count) noexcept;

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_OPS_H
