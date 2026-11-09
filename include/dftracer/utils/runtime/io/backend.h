// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_BACKEND_H

#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <csignal>
#include <cstddef>
#include <cstdint>

struct epoll_event;

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
    Call,
    Statx,
    Unlink,
    Rename,
    Mkdir,
    Fallocate,
    Fadvise,
    SyncFileRange,
    Splice,
    Openat2,
    Linkat,
    Symlinkat,
    Getxattr,
    Setxattr,
    Fgetxattr,
    Fsetxattr,
    Socket,
    Connect,
    Bind,
    Listen,
    Shutdown,
    Sendmsg,
    Recvmsg,
    Tee,
    Pipe,
    Madvise,
    Waitid,
    FutexWait,
    FutexWake,
    FutexWaitv,
    EpollCtl,
    EpollWait,
    PollAdd
};

/// The kernel's statx time, whatever the C library names it.
struct StatxTimestamp {
    std::int64_t sec;
    std::uint32_t nsec;
    std::int32_t reserved;
};

/// The kernel's struct statx, so that the layout does not depend on the C
/// library. @ref IoOpKind::Statx fills it.
struct Statx {
    std::uint32_t mask;
    std::uint32_t blksize;
    std::uint64_t attributes;
    std::uint32_t nlink;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint16_t mode;
    std::uint16_t spare0;
    std::uint64_t ino;
    std::uint64_t size;
    std::uint64_t blocks;
    std::uint64_t attributes_mask;
    StatxTimestamp atime;
    StatxTimestamp btime;
    StatxTimestamp ctime;
    StatxTimestamp mtime;
    std::uint32_t rdev_major;
    std::uint32_t rdev_minor;
    std::uint32_t dev_major;
    std::uint32_t dev_minor;
    std::uint64_t mnt_id;
    std::uint32_t dio_mem_align;
    std::uint32_t dio_offset_align;
    std::uint64_t spare3[12];
};
static_assert(sizeof(Statx) == 256, "Statx is the kernel's struct statx");

/// The kernel's struct open_how, which @ref IoOpKind::Openat2 reads.
struct OpenHow {
    std::uint64_t flags;
    std::uint64_t mode;
    std::uint64_t resolve;
};
static_assert(sizeof(OpenHow) == 24, "OpenHow is the kernel's struct open_how");

/// The kernel's struct futex_waitv: one futex that @ref IoOpKind::FutexWaitv
/// waits on. Its flags are those of @ref IO_FUTEX_SIZE_U32.
struct FutexWaitv {
    std::uint64_t value;
    std::uint64_t address;
    std::uint32_t flags;
    std::uint32_t reserved;
};
static_assert(sizeof(FutexWaitv) == 24,
              "FutexWaitv is the kernel's struct futex_waitv");

/// The futex flags of a 32 bit futex, and of one that only this process uses.
constexpr unsigned IO_FUTEX_SIZE_U32 = 0x02U;
constexpr unsigned IO_FUTEX_PRIVATE = 0x80U;

/// The mask that matches every waiter of a 32 bit futex.
constexpr std::uint64_t IO_FUTEX_MATCH_ANY = 0xffffffffU;

/// The arguments of the extended attribute operations. `value` is read by a
/// set and written by a get, with room for `size` bytes. The submitter keeps
/// it, and what it names, valid until the operation ends.
struct XattrArgs {
    const char* name;
    void* value;
    std::size_t size;
    int flags;
};

/// The mask for the fields every statx gives, as STATX_BASIC_STATS names them.
constexpr std::uint32_t STATX_BASIC_MASK = 0x7ffU;

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
        /// The second offset for @ref IoOpKind::Splice.
        std::int64_t out_offset;
        struct stat* stat_out;
        struct sockaddr* addr;
        BlockingFn call;
        /// The message of @ref IoOpKind::Sendmsg and @ref IoOpKind::Recvmsg.
        struct msghdr* msg;
        /// The result of @ref IoOpKind::Waitid.
        siginfo_t* info;
        /// The events of @ref IoOpKind::EpollCtl and @ref IoOpKind::EpollWait.
        struct epoll_event* epoll_events;
        /// The two descriptors that @ref IoOpKind::Pipe fills.
        int* pipe_fds;
        /// The word of @ref IoOpKind::FutexWait and @ref IoOpKind::FutexWake.
        std::uint32_t* futex;
        /// The futexes of @ref IoOpKind::FutexWaitv.
        const FutexWaitv* futex_waitv;
    };
    union {
        /// For @ref IoOpKind::Connect and @ref IoOpKind::Bind the size of the
        /// address, for @ref IoOpKind::FutexWaitv the futex count.
        std::size_t length = 0;
        socklen_t* addrlen;
        void* call_context;
        /// The new name for @ref IoOpKind::Rename.
        const char* path2;
        Statx* statx_out;
        const OpenHow* how;
        const XattrArgs* xattr;
        /// The id type of @ref IoOpKind::Waitid.
        int idtype;
        /// The mask of @ref IoOpKind::FutexWait and @ref IoOpKind::FutexWake.
        std::uint64_t futex_mask;
    };
    /// The position for a positional operation, the seek distance for
    /// @ref IoOpKind::Lseek and the new size for @ref IoOpKind::Ftruncate. -1
    /// for @ref IoOpKind::Splice uses the file position. The type for
    /// @ref IoOpKind::Socket, the expected value for @ref IoOpKind::FutexWait
    /// and the count for @ref IoOpKind::FutexWake, the descriptor for
    /// @ref IoOpKind::EpollCtl.
    std::int64_t offset = 0;
    IoCompleteFn complete = nullptr;
    void* context = nullptr;
    ssize_t result = 0;
    /// For the backend, while it holds the operation.
    IoOp* next = nullptr;
    IoOp* prev = nullptr;
    /// The source file for @ref IoOpKind::Sendfile, @ref IoOpKind::Splice and
    /// @ref IoOpKind::Tee, the directory for @ref IoOpKind::Statx,
    /// @ref IoOpKind::Openat2, @ref IoOpKind::Linkat and
    /// @ref IoOpKind::Symlinkat, the domain for @ref IoOpKind::Socket and the
    /// id for @ref IoOpKind::Waitid.
    dstd::NativeHandle handle = -1;
    /// For @ref IoOpKind::Unlink the flags of unlinkat, for
    /// @ref IoOpKind::Fallocate the mode, for @ref IoOpKind::Fadvise and
    /// @ref IoOpKind::Madvise the advice, for @ref IoOpKind::Socket the
    /// protocol, for @ref IoOpKind::Listen the backlog, for
    /// @ref IoOpKind::Shutdown the direction, for @ref IoOpKind::Waitid the
    /// options, for @ref IoOpKind::FutexWait and @ref IoOpKind::FutexWake the
    /// futex flags, for @ref IoOpKind::EpollCtl the operation, for
    /// @ref IoOpKind::EpollWait the most events and for @ref IoOpKind::PollAdd
    /// the events.
    union {
        int flags = 0;
        int iov_count;
        int whence;
    };
    /// The destination for @ref IoOpKind::Sendfile, @ref IoOpKind::Splice and
    /// @ref IoOpKind::Tee, the field mask for @ref IoOpKind::Statx, the new
    /// directory for @ref IoOpKind::Linkat.
    union {
        mode_t mode = 0;
        dstd::NativeHandle out_handle;
        std::uint32_t statx_mask;
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

    /// Fails when the backend cannot get what it needs to run.
    virtual dstd::Result<void> start() = 0;

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

    /// Called on a thread the executor starts, before its first operation, and
    /// again before it exits. A backend that ties operations to the thread that
    /// submits them uses these. An operation may come from any thread, attached
    /// or not.
    virtual void attach_thread() {}
    virtual void detach_thread() {}

    /// For a backend whose completions need the submitting thread. Gives a
    /// token when the calling thread should wait through @ref park instead of
    /// its own sleep, or null when it has nothing to wait for. A token needs a
    /// @ref finish_park on the same thread, whether it parked or not.
    virtual void* prepare_park() { return nullptr; }

    /// Waits for a completion on the calling thread, a @ref unpark or
    /// `timeout_ms`. It may also return early. Completions run in @ref
    /// finish_park.
    virtual void park(void* token, std::int64_t timeout_ms) {
        (void)token;
        (void)timeout_ms;
    }

    /// Ends a @ref park from any thread, or the next one with this token.
    virtual void unpark(void* token) noexcept { (void)token; }

    virtual void finish_park(void* token) { (void)token; }

    /// Human-readable name for logging.
    virtual dstd::String name() const = 0;
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_BACKEND_H
