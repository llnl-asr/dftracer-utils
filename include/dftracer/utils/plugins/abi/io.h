// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_IO_H
#define DFTRACER_UTILS_PLUGINS_ABI_IO_H

/** @file
 * dftu.svc.io: async POSIX file/socket I/O lent to a plugin. Optional service
 * group, fetched via dftu_plugin_host::get_service(DFTU_SVC_IO). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>
#include <sys/socket.h> /* struct sockaddr, socklen_t for accept */
#include <sys/uio.h>    /* struct iovec for the vectored ops */

#ifdef __cplusplus
extern "C" {
#endif

/** Stable stat subset, since struct stat layout is platform-dependent. */
typedef struct {
    uint64_t size;
    uint64_t mtime_ns;
    uint32_t mode;
} dftu_stat;

#define DFTU_SVC_IO "dftu.svc.io"

/** Each call starts an operation and returns its dftu_task in `*out` for the
 * host to await. The result slot (fd, byte count, offset, stat) must outlive
 * the task and holds only a non-negative result. A failed operation finishes
 * its task with DFTU_E_IO (DFTU_E_NOT_FOUND for ENOENT, DFTU_E_INVALID for
 * EINVAL and EBADF) and the message "<op>: <strerror> (errno N)". `open`
 * copies `path` when called, so the plugin's buffer need not outlive the call.
 */
typedef struct dftu_io {
    uint32_t struct_size; /**< sizeof(dftu_io) */
    dftu_status (*open)(dftu_ctx* ctx, void* h, const char* path, int32_t flags,
                        int32_t mode, int32_t* out_fd, dftu_task** out);
    dftu_status (*close)(dftu_ctx* ctx, void* h, int32_t fd, dftu_task** out);
    dftu_status (*read)(dftu_ctx* ctx, void* h, int32_t fd, void* buf,
                        uint64_t len, int64_t* out_n, dftu_task** out);
    dftu_status (*write)(dftu_ctx* ctx, void* h, int32_t fd, const void* buf,
                         uint64_t len, int64_t* out_n, dftu_task** out);
    dftu_status (*pread)(dftu_ctx* ctx, void* h, int32_t fd, void* buf,
                         uint64_t len, uint64_t off, int64_t* out_n,
                         dftu_task** out);
    dftu_status (*pwrite)(dftu_ctx* ctx, void* h, int32_t fd, const void* buf,
                          uint64_t len, uint64_t off, int64_t* out_n,
                          dftu_task** out);
    dftu_status (*fsync)(dftu_ctx* ctx, void* h, int32_t fd, dftu_task** out);
    dftu_status (*ftruncate)(dftu_ctx* ctx, void* h, int32_t fd, uint64_t len,
                             dftu_task** out);
    dftu_status (*fstat)(dftu_ctx* ctx, void* h, int32_t fd, dftu_stat* out_st,
                         dftu_task** out);
    /** The vectored ops take a borrowed iovec array valid until the task
       finishes; out_n receives the byte count. */
    dftu_status (*readv)(dftu_ctx* ctx, void* h, int32_t fd,
                         const struct iovec* iov, int32_t iovcnt,
                         int64_t* out_n, dftu_task** out);
    dftu_status (*writev)(dftu_ctx* ctx, void* h, int32_t fd,
                          const struct iovec* iov, int32_t iovcnt,
                          int64_t* out_n, dftu_task** out);
    dftu_status (*preadv)(dftu_ctx* ctx, void* h, int32_t fd,
                          const struct iovec* iov, int32_t iovcnt, uint64_t off,
                          int64_t* out_n, dftu_task** out);
    dftu_status (*pwritev)(dftu_ctx* ctx, void* h, int32_t fd,
                           const struct iovec* iov, int32_t iovcnt,
                           uint64_t off, int64_t* out_n, dftu_task** out);
    /** Reposition the file offset; whence is SEEK_SET/CUR/END. out_off
       receives the resulting absolute offset. */
    dftu_status (*lseek)(dftu_ctx* ctx, void* h, int32_t fd, int64_t off,
                         int32_t whence, int64_t* out_off, dftu_task** out);
    /** Zero-copy transfer of `count` bytes from in_fd (a regular file) to
       out_fd starting at `off`; out_n receives the bytes sent. */
    dftu_status (*sendfile)(dftu_ctx* ctx, void* h, int32_t out_fd,
                            int32_t in_fd, uint64_t off, uint64_t count,
                            int64_t* out_n, dftu_task** out);
    /** Accept a connection on a listening socket; addr/addrlen may be NULL.
       out_fd receives the client fd. */
    dftu_status (*accept)(dftu_ctx* ctx, void* h, int32_t fd,
                          struct sockaddr* addr, socklen_t* addrlen,
                          int32_t* out_fd, dftu_task** out);
    /** Socket receive/send; flags are the recv(2)/send(2) flags. out_n
       receives the byte count. */
    dftu_status (*recv)(dftu_ctx* ctx, void* h, int32_t fd, void* buf,
                        uint64_t len, int32_t flags, int64_t* out_n,
                        dftu_task** out);
    dftu_status (*send)(dftu_ctx* ctx, void* h, int32_t fd, const void* buf,
                        uint64_t len, int32_t flags, int64_t* out_n,
                        dftu_task** out);
} dftu_io;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_IO_H */
