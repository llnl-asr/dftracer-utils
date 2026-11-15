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

/** A statx result with fixed-width fields. Times are nanoseconds since the
 * epoch; the fields the kernel did not fill are zero, and `mask` says which it
 * filled. */
typedef struct {
    uint64_t attributes;
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    int64_t atime_ns;
    int64_t btime_ns;
    int64_t ctime_ns;
    int64_t mtime_ns;
    uint32_t mask;
    uint32_t blksize;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t rdev_major;
    uint32_t rdev_minor;
    uint32_t dev_major;
    uint32_t dev_minor;
} dftu_statx;

/** The arguments of openat2, as the kernel's struct open_how names them. */
typedef struct {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
} dftu_open_how;

#define DFTU_SVC_IO "dftu.svc.io"

/** Each call starts an operation and returns its dftu_task in `*out` for the
 * host to await. The result slot (fd, byte count, offset, stat) must outlive
 * the task and holds only a non-negative result. A failed operation finishes
 * its task with DFTU_E_IO (DFTU_E_NOT_FOUND for ENOENT, DFTU_E_INVALID for
 * EINVAL and EBADF) and the message "<op>: <strerror> (errno N)". `open`
 * copies `path` when called, so the plugin's buffer need not outlive the call.
 * Every call after `send` copies the paths, names and socket addresses it
 * gets in the same way. A buffer, message header or result slot that a call
 * names (value, msg, out_*, fds) must stay valid until the task finishes.
 * The ops that need Linux (statx, fallocate, sync_file_range, splice, openat2,
 * the four xattr calls, tee, pipe) return DFTU_E_UNSUPPORTED, with no task, on
 * another platform. An errno of ENOSYS, EOPNOTSUPP or ENOTSUP fails a task with
 * DFTU_E_UNSUPPORTED.
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
    dftu_status (*unlink)(dftu_ctx* ctx, void* h, const char* path,
                          dftu_task** out);
    dftu_status (*rmdir)(dftu_ctx* ctx, void* h, const char* path,
                         dftu_task** out);
    dftu_status (*rename)(dftu_ctx* ctx, void* h, const char* from,
                          const char* to, dftu_task** out);
    dftu_status (*mkdir)(dftu_ctx* ctx, void* h, const char* path, int32_t mode,
                         dftu_task** out);
    /** `advice` is a POSIX_FADV_* value; a platform without fadvise succeeds
       and does nothing. */
    dftu_status (*fadvise)(dftu_ctx* ctx, void* h, int32_t fd, uint64_t off,
                           uint64_t len, int32_t advice, dftu_task** out);
    dftu_status (*linkat)(dftu_ctx* ctx, void* h, int32_t from_dirfd,
                          const char* from, int32_t to_dirfd, const char* to,
                          int32_t flags, dftu_task** out);
    dftu_status (*symlinkat)(dftu_ctx* ctx, void* h, const char* target,
                             int32_t dirfd, const char* link, dftu_task** out);
    /** out_fd receives the new socket. */
    dftu_status (*socket)(dftu_ctx* ctx, void* h, int32_t domain, int32_t type,
                          int32_t protocol, int32_t* out_fd, dftu_task** out);
    dftu_status (*connect)(dftu_ctx* ctx, void* h, int32_t fd,
                           const struct sockaddr* addr, socklen_t addrlen,
                           dftu_task** out);
    dftu_status (*bind)(dftu_ctx* ctx, void* h, int32_t fd,
                        const struct sockaddr* addr, socklen_t addrlen,
                        dftu_task** out);
    dftu_status (*listen)(dftu_ctx* ctx, void* h, int32_t fd, int32_t backlog,
                          dftu_task** out);
    /** `how` is SHUT_RD, SHUT_WR or SHUT_RDWR. */
    dftu_status (*shutdown)(dftu_ctx* ctx, void* h, int32_t fd, int32_t how,
                            dftu_task** out);
    /** `msg` and the buffers it names stay valid until the task finishes;
       out_n receives the byte count. */
    dftu_status (*sendmsg)(dftu_ctx* ctx, void* h, int32_t fd,
                           const struct msghdr* msg, int32_t flags,
                           int64_t* out_n, dftu_task** out);
    dftu_status (*recvmsg)(dftu_ctx* ctx, void* h, int32_t fd,
                           struct msghdr* msg, int32_t flags, int64_t* out_n,
                           dftu_task** out);
    /** Waits once for `events` (POLLIN and the like) on `fd`; out_events
       receives the events that happened. */
    dftu_status (*poll_add)(dftu_ctx* ctx, void* h, int32_t fd, uint32_t events,
                            uint32_t* out_events, dftu_task** out);
    /** `dirfd` is AT_FDCWD for a path that is not relative to a directory. */
    dftu_status (*statx)(dftu_ctx* ctx, void* h, int32_t dirfd,
                         const char* path, int32_t flags, uint32_t mask,
                         dftu_statx* out_st, dftu_task** out);
    dftu_status (*fallocate)(dftu_ctx* ctx, void* h, int32_t fd, int32_t mode,
                             uint64_t off, uint64_t len, dftu_task** out);
    dftu_status (*sync_file_range)(dftu_ctx* ctx, void* h, int32_t fd,
                                   uint64_t off, uint64_t len, uint32_t flags,
                                   dftu_task** out);
    /** A NULL in_off or out_off is the file position, as -1 is in splice(2);
       a non-NULL offset is read when the call starts and advanced by the bytes
       moved when the task ends. out_n receives the bytes moved. */
    dftu_status (*splice)(dftu_ctx* ctx, void* h, int32_t in_fd,
                          int64_t* in_off, int32_t out_fd, int64_t* out_off,
                          uint64_t len, uint32_t flags, int64_t* out_n,
                          dftu_task** out);
    /** out_fd receives the descriptor. */
    dftu_status (*openat2)(dftu_ctx* ctx, void* h, int32_t dirfd,
                           const char* path, const dftu_open_how* how,
                           int32_t* out_fd, dftu_task** out);
    /** A get fills `value`, which has room for `size` bytes, and gives the
       size of the attribute in out_n. A set reads `size` bytes of `value`. */
    dftu_status (*getxattr)(dftu_ctx* ctx, void* h, const char* path,
                            const char* name, void* value, uint64_t size,
                            int64_t* out_n, dftu_task** out);
    dftu_status (*setxattr)(dftu_ctx* ctx, void* h, const char* path,
                            const char* name, const void* value, uint64_t size,
                            int32_t flags, dftu_task** out);
    dftu_status (*fgetxattr)(dftu_ctx* ctx, void* h, int32_t fd,
                             const char* name, void* value, uint64_t size,
                             int64_t* out_n, dftu_task** out);
    dftu_status (*fsetxattr)(dftu_ctx* ctx, void* h, int32_t fd,
                             const char* name, const void* value, uint64_t size,
                             int32_t flags, dftu_task** out);
    dftu_status (*tee)(dftu_ctx* ctx, void* h, int32_t in_fd, int32_t out_fd,
                       uint64_t len, uint32_t flags, int64_t* out_n,
                       dftu_task** out);
    /** fds receives the read end then the write end. */
    dftu_status (*pipe)(dftu_ctx* ctx, void* h, int32_t fds[2], int32_t flags,
                        dftu_task** out);
} dftu_io;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_IO_H */
