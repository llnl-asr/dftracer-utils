#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_OP_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_OP_H

namespace dftracer::utils::io {

/// I/O operation types, shared by every backend (thread pool, epoll, kqueue,
/// io_uring) so the op set is maintained in one place.
enum class IoOp {
    Read,
    Write,
    Pread,
    Pwrite,
    Open,
    Close,
    Fsync,
    Ftruncate,
    Fstat,
    Accept,
    Recv,
    Send,
    Readv,
    Writev,
    Preadv,
    Pwritev,
    Lseek,
    Sendfile
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_OP_H
