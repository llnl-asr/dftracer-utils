#ifndef DFTRACER_UTILS_CORE_IO_OPS_H
#define DFTRACER_UTILS_CORE_IO_OPS_H

#include <dftracer/utils/core/io/awaitable.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>

namespace dftracer::utils::io {

/// Asynchronous read. If inside an executor worker, submits to the I/O
/// backend and suspends. Otherwise falls back to blocking pread().
IoAwaitable read(int fd, void* buf, std::size_t len, off_t offset = 0) noexcept;

/// Asynchronous write.
IoAwaitable write(int fd, const void* buf, std::size_t len,
                  off_t offset = 0) noexcept;

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

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_OPS_H
