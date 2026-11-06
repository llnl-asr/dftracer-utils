// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_BACKEND_H

#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::runtime::io {

/// Every operation a backend runs.
enum class IoOpKind : std::uint8_t {
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
    Sendfile,
    Call
};

struct IoOp;

/// Runs when an operation ends. It must not throw. It may destroy the operation
/// or submit it again.
using IoCompleteFn = void (*)(IoOp& op) noexcept;

/// A blocking call for @ref IoOpKind::Call to run. It must not throw. A failure
/// returns a negative result with errno set, as a system call does.
using BlockingFn = ssize_t (*)(void* context) noexcept;

/**
 * @brief One I/O operation. The submitter owns it, so a submit allocates
 * nothing.
 *
 * It stays valid and in place from @ref IoBackend::submit until @ref complete
 * runs. A backend reads the fields of its kind, sets @ref result to the value
 * the system call gives or to a negative errno, calls @ref complete once from
 * any thread and does not touch the operation again.
 */
struct IoOp {
    union {
        void* buffer = nullptr;
        const struct iovec* iov;
        const char* path;
        struct stat* stat_out;
        struct sockaddr* addr;
        BlockingFn call;
    };
    union {
        std::size_t length = 0;
        socklen_t* addrlen;
        void* call_context;
    };
    /// The position for a positional operation, the seek distance for
    /// @ref IoOpKind::Lseek and the new size for @ref IoOpKind::Ftruncate.
    std::int64_t offset = 0;
    IoCompleteFn complete = nullptr;
    void* context = nullptr;
    ssize_t result = 0;
    /// For the backend, while it holds the operation.
    IoOp* next = nullptr;
    IoOp* prev = nullptr;
    /// The source file for @ref IoOpKind::Sendfile.
    dstd::NativeHandle handle = -1;
    union {
        int flags = 0;
        int iov_count;
        int whence;
    };
    union {
        mode_t mode = 0;
        dstd::NativeHandle out_handle;
    };
    IoOpKind kind = IoOpKind::Read;
};

/**
 * @brief How operations run: on threads, on a kernel ring or anything else.
 *
 * submit, flush, in_flight and cancel_pending are thread safe.
 */
class IoBackend {
   public:
    virtual ~IoBackend() = default;

    virtual void start() = 0;

    /// Call it after in_flight() reads zero. An operation still running then
    /// never completes.
    virtual void stop() = 0;

    /// Takes an operation. It need not start before the next @ref flush.
    virtual void submit(IoOp& op) = 0;

    /// Starts every operation submitted so far.
    virtual int flush() { return 0; }

    /// Lets an idle worker reap completions. Gives the count it reaped.
    virtual std::size_t poll(int timeout_ms = 0) = 0;

    /// Raises the cap on the threads that run blocking operations to
    /// `threads`. A smaller count changes nothing. `eager` true starts every
    /// thread up to the cap. False for a backend with no such threads.
    virtual bool grow_threads(std::size_t threads, bool eager) {
        (void)threads;
        (void)eager;
        return false;
    }

    /// File operations still in flight. The executor waits for zero before it
    /// stops, so a read that finishes after the workers are gone does not
    /// resume into a destroyed queue.
    virtual std::size_t in_flight() const { return 0; }

    /// Cancel every operation that can wait on a peer and fail any submitted
    /// later. Each one completes with -ECANCELED, so in_flight() falls to zero
    /// without waiting for the peer of a pipe or socket. Sticky until stop().
    /// Called by executor shutdown.
    virtual void cancel_pending() {}

    /// Human-readable name for logging.
    virtual dstd::String name() const = 0;
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_BACKEND_H
