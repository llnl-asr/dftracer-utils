#ifndef DFTRACER_UTILS_CORE_IO_IO_OP_H
#define DFTRACER_UTILS_CORE_IO_IO_OP_H

namespace dftracer::utils::io {

/// I/O operation types, shared by every backend (thread pool, epoll, kqueue,
/// io_uring) so the op set is maintained in one place.
enum class IoOp {
    READ,
    WRITE,
    PREAD,
    PWRITE,
    OPEN,
    CLOSE,
    FSYNC,
    FTRUNCATE,
    FSTAT,
    ACCEPT,
    RECV,
    SEND,
    READV,
    WRITEV,
    PREADV,
    PWRITEV,
    LSEEK,
    SENDFILE
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_IO_OP_H
