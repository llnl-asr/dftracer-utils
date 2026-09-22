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

#define DFTU_SVC_IO "dftu.svc.io@1"

/** Each call returns a dftu_task to compose/await; the out-slot must outlive
 * it. `open` copies `path` when called, so the plugin's buffer need not.
 */
typedef struct dftu_io {
    dftu_task* (*open)(void* h, const char* path, int flags, int mode,
                       int* out_fd);
    dftu_task* (*close)(void* h, int fd, int* out_rc);
    dftu_task* (*read)(void* h, int fd, void* buf, uint64_t len,
                       int64_t* out_n);
    dftu_task* (*write)(void* h, int fd, const void* buf, uint64_t len,
                        int64_t* out_n);
    dftu_task* (*pread)(void* h, int fd, void* buf, uint64_t len, uint64_t off,
                        int64_t* out_n);
    dftu_task* (*pwrite)(void* h, int fd, const void* buf, uint64_t len,
                         uint64_t off, int64_t* out_n);
    dftu_task* (*fsync)(void* h, int fd, int* out_rc);
    dftu_task* (*ftruncate)(void* h, int fd, uint64_t len, int* out_rc);
    dftu_task* (*fstat)(void* h, int fd, dftu_stat* out);
    /** Appended after the initial 9 ops; append-only within dft.ext.io@1, so a
       host predating one of these leaves its slot NULL. The vectored ops take a
       borrowed iovec array valid for the await; out_n receives the byte count
       (negative errno on failure). */
    dftu_task* (*readv)(void* h, int fd, const struct iovec* iov, int iovcnt,
                        int64_t* out_n);
    dftu_task* (*writev)(void* h, int fd, const struct iovec* iov, int iovcnt,
                         int64_t* out_n);
    dftu_task* (*preadv)(void* h, int fd, const struct iovec* iov, int iovcnt,
                         uint64_t off, int64_t* out_n);
    dftu_task* (*pwritev)(void* h, int fd, const struct iovec* iov, int iovcnt,
                          uint64_t off, int64_t* out_n);
    /** Reposition the file offset; whence is SEEK_SET/CUR/END. out_off receives
       the resulting absolute offset, or a negative errno. */
    dftu_task* (*lseek)(void* h, int fd, int64_t off, int whence,
                        int64_t* out_off);
    /** Zero-copy transfer of `count` bytes from in_fd (a regular file) to
       out_fd starting at `off`; out_n receives the bytes sent. */
    dftu_task* (*sendfile)(void* h, int out_fd, int in_fd, uint64_t off,
                           uint64_t count, int64_t* out_n);
    /** Accept a connection on a listening socket; addr/addrlen may be NULL.
       out_fd receives the client fd, or a negative errno. */
    dftu_task* (*accept)(void* h, int fd, struct sockaddr* addr,
                         socklen_t* addrlen, int* out_fd);
    /** Socket receive/send; flags are the recv(2)/send(2) flags. out_n receives
       the byte count, or a negative errno. */
    dftu_task* (*recv)(void* h, int fd, void* buf, uint64_t len, int flags,
                       int64_t* out_n);
    dftu_task* (*send)(void* h, int fd, const void* buf, uint64_t len,
                       int flags, int64_t* out_n);
} dftu_io;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_IO_H */
