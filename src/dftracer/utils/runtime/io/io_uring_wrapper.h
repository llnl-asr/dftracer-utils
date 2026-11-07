// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_URING_WRAPPER_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_URING_WRAPPER_H
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <linux/io_uring.h>
// linux/io_uring.h may pull in kernel headers that define BLOCK_SIZE
// as a macro, which collides with concurrentqueue. Undefine it.
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef IORING_FEAT_EXT_ARG
#define IORING_FEAT_EXT_ARG (1U << 8)
#endif
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 3)
#endif
#ifndef IORING_SETUP_IOPOLL
#define IORING_SETUP_IOPOLL (1U << 0)
#endif
#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL (1U << 1)
#endif
#ifndef IORING_SETUP_SQ_AFF
#define IORING_SETUP_SQ_AFF (1U << 2)
#endif
#ifndef IORING_SETUP_CQSIZE
#define IORING_SETUP_CQSIZE (1U << 3)
#endif
#ifndef IORING_SETUP_CLAMP
#define IORING_SETUP_CLAMP (1U << 4)
#endif
#ifndef IORING_SETUP_ATTACH_WQ
#define IORING_SETUP_ATTACH_WQ (1U << 5)
#endif
#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_SQE128
#define IORING_SETUP_SQE128 (1U << 10)
#endif
#ifndef IORING_SETUP_CQE32
#define IORING_SETUP_CQE32 (1U << 11)
#endif
#ifndef IORING_SETUP_NO_MMAP
#define IORING_SETUP_NO_MMAP (1U << 14)
#endif
#ifndef IORING_SETUP_REGISTERED_FD_ONLY
#define IORING_SETUP_REGISTERED_FD_ONLY (1U << 15)
#endif
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif
#ifndef IORING_SETUP_HYBRID_IOPOLL
#define IORING_SETUP_HYBRID_IOPOLL (1U << 17)
#endif
#ifndef IORING_SETUP_CQE_MIXED
#define IORING_SETUP_CQE_MIXED (1U << 18)
#endif
#ifndef IORING_SETUP_SQE_MIXED
#define IORING_SETUP_SQE_MIXED (1U << 19)
#endif
#ifndef IORING_SETUP_SQ_REWIND
#define IORING_SETUP_SQ_REWIND (1U << 20)
#endif
#ifndef IORING_ENTER_REGISTERED_RING
#define IORING_ENTER_REGISTERED_RING (1U << 4)
#endif
#ifndef IORING_CQE_F_SKIP
#define IORING_CQE_F_SKIP (1U << 5)
#endif
#ifndef IORING_CQE_F_32
#define IORING_CQE_F_32 (1U << 15)
#endif
#ifndef IORING_SETUP_R_DISABLED
#define IORING_SETUP_R_DISABLED (1U << 6)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_FEAT_SQPOLL_NONFIXED
#define IORING_FEAT_SQPOLL_NONFIXED (1U << 7)
#endif
#ifndef IORING_ENTER_SQ_WAIT
#define IORING_ENTER_SQ_WAIT (1U << 2)
#endif
#ifndef IORING_SQ_CQ_OVERFLOW
#define IORING_SQ_CQ_OVERFLOW (1U << 1)
#endif
#ifndef IORING_SQ_TASKRUN
#define IORING_SQ_TASKRUN (1U << 2)
#endif
#ifndef IORING_REGISTER_ENABLE_RINGS
#define IORING_REGISTER_ENABLE_RINGS 12
#endif

namespace dftracer::utils::runtime::io::uring {

// Kernel ABI values that older kernel headers do not name.
constexpr unsigned REGISTER_PROBE = 8;
constexpr unsigned UNREGISTER_RING_FDS = 21;
constexpr unsigned REGISTER_USE_REGISTERED_RING = 1U << 31;
constexpr unsigned REGISTER_IOWQ_MAX_WORKERS = 19;

// The opcodes are enum values in the kernel header, which an older header lacks
// and a macro test cannot see. These are its numbers, after
// https://github.com/axboe/liburing/blob/master/src/include/liburing/io_uring.h.
constexpr unsigned OP_POLL_ADD = 6;
constexpr unsigned OP_SYNC_FILE_RANGE = 8;
constexpr unsigned OP_SENDMSG = 9;
constexpr unsigned OP_RECVMSG = 10;
constexpr unsigned OP_CONNECT = 16;
constexpr unsigned OP_FALLOCATE = 17;
constexpr unsigned OP_STATX = 21;
constexpr unsigned OP_FADVISE = 24;
constexpr unsigned OP_MADVISE = 25;
constexpr unsigned OP_OPENAT2 = 28;
constexpr unsigned OP_EPOLL_CTL = 29;
constexpr unsigned OP_SPLICE = 30;
constexpr unsigned OP_TEE = 33;
constexpr unsigned OP_SHUTDOWN = 34;
constexpr unsigned OP_RENAMEAT = 35;
constexpr unsigned OP_UNLINKAT = 36;
constexpr unsigned OP_MKDIRAT = 37;
constexpr unsigned OP_SYMLINKAT = 38;
constexpr unsigned OP_LINKAT = 39;
constexpr unsigned OP_FSETXATTR = 41;
constexpr unsigned OP_SETXATTR = 42;
constexpr unsigned OP_FGETXATTR = 43;
constexpr unsigned OP_GETXATTR = 44;
constexpr unsigned OP_SOCKET = 45;
constexpr unsigned OP_WAITID = 50;
constexpr unsigned OP_FUTEX_WAIT = 51;
constexpr unsigned OP_FUTEX_WAKE = 52;
constexpr unsigned OP_FUTEX_WAITV = 53;
constexpr unsigned OP_FTRUNCATE = 55;
constexpr unsigned OP_BIND = 56;
constexpr unsigned OP_LISTEN = 57;
constexpr unsigned OP_EPOLL_WAIT = 59;
constexpr unsigned OP_PIPE = 62;

// The bytes of an SQE that hold splice_fd_in, file_index and addr3, which a
// header before 5.7 lacks or names otherwise.
constexpr std::size_t SQE_SPLICE_FD_IN_OFFSET = 44;
constexpr std::size_t SQE_FILE_INDEX_OFFSET = 44;
constexpr std::size_t SQE_ADDR3_OFFSET = 48;

constexpr unsigned PROBE_OP_SUPPORTED = 1U << 0;
constexpr unsigned PROBE_OPS = 256;

// The kernel's struct io_uring_probe, with the room for every opcode.
struct ProbeOp {
    std::uint8_t op;
    std::uint8_t resv;
    std::uint16_t flags;
    std::uint32_t resv2;
};

struct Probe {
    std::uint8_t last_op;
    std::uint8_t ops_len;
    std::uint16_t resv;
    std::uint32_t resv2[3];
    ProbeOp ops[PROBE_OPS];

    bool has(unsigned opcode) const {
        return opcode <= static_cast<unsigned>(last_op) && opcode < PROBE_OPS &&
               (ops[opcode].flags & PROBE_OP_SUPPORTED) != 0;
    }
};

struct KernelTimespec {
    std::int64_t tv_sec;
    long long tv_nsec;
};

struct GeteventsArg {
    std::uint64_t sigmask;
    std::uint32_t sigmask_sz;
    std::uint32_t pad;
    std::uint64_t ts;
};

// ============================================================================
// Raw syscall wrappers
// ============================================================================

inline int sys_io_uring_setup(unsigned entries, struct io_uring_params* p) {
    return static_cast<int>(syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                              unsigned flags) {
    return static_cast<int>(syscall(__NR_io_uring_enter, fd, to_submit,
                                    min_complete, flags, nullptr, 0));
}

inline int sys_io_uring_register(int fd, unsigned opcode, void* arg,
                                 unsigned nr_args) {
    return static_cast<int>(
        syscall(__NR_io_uring_register, fd, opcode, arg, nr_args));
}

// ============================================================================
// Minimal io_uring ring manager
// ============================================================================

#ifndef IOSQE_CQE_SKIP_SUCCESS
#define IOSQE_CQE_SKIP_SUCCESS (1U << 6)
#endif

/// The kernel's struct io_uring_rsrc_update.
struct RsrcUpdate {
    std::uint32_t offset = 0;
    std::uint32_t resv = 0;
    std::uint64_t data = 0;
};
static_assert(sizeof(RsrcUpdate) == 16);

/// What @ref Ring::init asks of the kernel. A zero field asks for nothing.
struct RingOptions {
    /// IORING_SETUP_* bits.
    unsigned flags = 0;
    /// With IORING_SETUP_SQPOLL, the time without work before the kernel
    /// thread sleeps.
    unsigned sq_idle_ms = 0;
    /// With IORING_SETUP_CQSIZE, the CQ size.
    unsigned cq_entries = 0;
    /// With IORING_SETUP_SQ_AFF, the CPU of the kernel thread.
    unsigned sq_cpu = 0;
    /// With IORING_SETUP_ATTACH_WQ, the ring whose kernel workers to share.
    int attach_fd = -1;
};

/// Owns the ring fd and mmap'd memory. One thread submits. Completions need one
/// reader at a time, which may be another thread.
class Ring {
    int ring_fd_ = -1;
    // ring_fd_, or the index that @ref enter_through_index was given.
    int enter_fd_ = -1;
    unsigned features_ = 0;
    unsigned cq_entries_ = 0;

    // SQ ring (submission queue)
    void* sq_ring_ptr_ = nullptr;
    std::size_t sq_ring_size_ = 0;
    unsigned* sq_head_ = nullptr;
    unsigned* sq_tail_ = nullptr;
    unsigned* sq_ring_mask_ = nullptr;
    unsigned* sq_ring_entries_ = nullptr;
    unsigned* sq_flags_ = nullptr;
    // The tail with the SQEs being filled. The kernel sees it at the next
    // enter, so it never reads a half-written SQE.
    unsigned sqe_tail_ = 0;
    bool is_sq_polled_ = false;
    bool deferred_ = false;
    // IOPOLL finds its completions only inside an enter.
    bool is_iopoll_ = false;
    // Each entry spans 1 << shift slots of the mapped array.
    unsigned sqe_shift_ = 0;
    unsigned cqe_shift_ = 0;
    bool is_sqe_mixed_ = false;
    bool is_cqe_mixed_ = false;
    bool is_sq_rewound_ = false;
    bool is_fd_registered_ = false;
    // IORING_ENTER_REGISTERED_RING when ring_fd_ is a registered index.
    unsigned enter_flags_ = 0;

    // CQ ring (completion queue)
    void* cq_ring_ptr_ = nullptr;
    std::size_t cq_ring_size_ = 0;
    unsigned* cq_head_ = nullptr;
    unsigned* cq_tail_ = nullptr;
    unsigned* cq_ring_mask_ = nullptr;
    struct io_uring_cqe* cqes_ = nullptr;
    // For the one reader. The kernel sees the new head once per batch.
    unsigned cq_seen_ = 0;
    unsigned cq_committed_ = 0;
    unsigned cq_visible_tail_ = 0;

    // SQE array
    struct io_uring_sqe* sqes_ = nullptr;
    std::size_t sqes_size_ = 0;

    // SQEs prepared but not yet taken by the kernel.
    unsigned pending_ = 0;

    // IORING_SETUP_NO_MMAP only. The rings' memory, as one region.
    std::size_t own_ring_size_ = 0;

    bool map_own_memory(unsigned entries, const RingOptions& options,
                        struct io_uring_params& params);
    void unmap_own_memory();
    void publish_sqes();
    int enter_polled(bool wait);

   public:
    Ring() = default;
    ~Ring() { destroy(); }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    /// Initialize the ring. Returns true on success, false if kernel
    /// doesn't support io_uring (ENOSYS) or other setup failure.
    /// With IORING_SETUP_SQPOLL a kernel thread takes the SQEs and sleeps after
    /// `sq_idle_ms` with nothing to take.
    bool init(unsigned entries, unsigned setup_flags = 0,
              unsigned sq_idle_ms = 0);

    /// As above with every setup parameter. IORING_SETUP_NO_MMAP rings use
    /// anonymous memory of one page or one huge page each, and fail when the
    /// system has no huge page to give.
    bool init(unsigned entries, const RingOptions& options);

    /// Makes the calling thread the ring's one submitter.
    bool enable();

    /// Moves completions that wait in the kernel, deferred or past a full CQ,
    /// into the CQ. Only the submitting thread may call it.
    void flush_completions();

    /// Tear down: munmap + close fd.
    void destroy();

    unsigned features() const { return features_; }
    unsigned cq_entries() const { return cq_entries_; }
    /// The ring descriptor, or its registered index when
    /// @ref is_fd_registered holds.
    int fd() const { return ring_fd_; }
    bool is_fd_registered() const { return is_fd_registered_; }

    /// Enters through `index`, which IORING_REGISTER_RING_FDS gave for this
    /// ring on the calling thread, so an enter takes no descriptor reference.
    /// Only that thread may then enter.
    void enter_through_index(unsigned index);

    /// A register call on this ring, on either form of descriptor. Gives the
    /// kernel's value or -1 with errno set.
    int register_call(unsigned opcode, void* arg, unsigned count);

    /// Next free SQE, which counts as pending once prepared. Null if the SQ
    /// is full.
    struct io_uring_sqe* get_sqe();

    /// A 128 byte entry, on a ring with IORING_SETUP_SQE128 or
    /// IORING_SETUP_SQE_MIXED. On a mixed ring it pads with a NOP that posts
    /// nothing when the entry would wrap. Null if the SQ has no room.
    struct io_uring_sqe* get_sqe128();

    unsigned pending() const { return pending_; }

    /// Hands the pending SQEs to the kernel and, when `wait` is set, waits for
    /// one completion. Gives the count the kernel took or a negative errno. The
    /// ones it did not take stay pending. On a polled ring it enters only to
    /// wake the kernel thread, wait for a completion or wait for SQ room.
    int enter(bool wait);

    /// As enter(true), giving up after `timeout_ms`, then with -ETIME.
    int enter_wait(std::int64_t timeout_ms);

    /// Asks the kernel which opcodes it has. False when it refuses, as a kernel
    /// before 5.6 does.
    bool probe(Probe& out);

    /// Caps the kernel threads that serve the calling thread's bounded work.
    void limit_kernel_workers(unsigned bounded);

    /// Whether a completion waits. Any thread may ask. It can say true for one
    /// that the reader has taken but not yet committed.
    bool has_cqe() const;

    /// Next completion, or null. Copy what it holds before cqe_seen(). The
    /// reader must call until it gives null, which tells the kernel the room is
    /// free again.
    struct io_uring_cqe* peek_cqe();

    void cqe_seen();
};

// ============================================================================
// SQE preparation helpers, after the prep helpers and ring setup of liburing,
// https://github.com/axboe/liburing (MIT), Copyright 2020 Jens Axboe, file
// src/include/liburing.h.
// ============================================================================

inline void prep_read(struct io_uring_sqe* sqe, int fd, void* buf, unsigned len,
                      off_t offset) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = len;
    sqe->off = static_cast<__u64>(offset);
}

inline void prep_write(struct io_uring_sqe* sqe, int fd, const void* buf,
                       unsigned len, off_t offset) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = len;
    sqe->off = static_cast<__u64>(offset);
}

inline void prep_openat(struct io_uring_sqe* sqe, int dfd, const char* path,
                        int flags, mode_t mode) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_OPENAT;
    sqe->fd = dfd;
    sqe->addr = reinterpret_cast<__u64>(path);
    sqe->len = static_cast<__u32>(mode);
    sqe->open_flags = static_cast<__u32>(flags);
}

inline void prep_nop(struct io_uring_sqe* sqe) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_NOP;
}

// Cancels the request whose user_data is `target`.
inline void prep_cancel(struct io_uring_sqe* sqe, void* target) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = -1;
    sqe->addr = reinterpret_cast<__u64>(target);
}

// Ends with -ETIME after `timeout`, which must live until the kernel takes the
// request.
inline void prep_timeout(struct io_uring_sqe* sqe,
                         const KernelTimespec* timeout) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->fd = -1;
    sqe->addr = reinterpret_cast<__u64>(timeout);
    sqe->len = 1;
}

inline void prep_close(struct io_uring_sqe* sqe, int fd) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_CLOSE;
    sqe->fd = fd;
}

inline void prep_fsync(struct io_uring_sqe* sqe, int fd, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_FSYNC;
    sqe->fd = fd;
    sqe->fsync_flags = flags;
}

inline void prep_accept(struct io_uring_sqe* sqe, int fd, struct sockaddr* addr,
                        socklen_t* addrlen, int flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(addr);
    sqe->addr2 = reinterpret_cast<__u64>(addrlen);
    sqe->accept_flags = static_cast<__u32>(flags);
}

inline void prep_recv(struct io_uring_sqe* sqe, int fd, void* buf, unsigned len,
                      int flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = len;
    sqe->msg_flags = static_cast<__u32>(flags);
}

inline void prep_send(struct io_uring_sqe* sqe, int fd, const void* buf,
                      unsigned len, int flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = len;
    sqe->msg_flags = static_cast<__u32>(flags);
}

inline void prep_readv(struct io_uring_sqe* sqe, int fd,
                       const struct iovec* iov, unsigned nr_vecs,
                       off_t offset) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READV;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(iov);
    sqe->len = nr_vecs;
    sqe->off = static_cast<__u64>(offset);
}

inline void prep_writev(struct io_uring_sqe* sqe, int fd,
                        const struct iovec* iov, unsigned nr_vecs,
                        off_t offset) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_WRITEV;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(iov);
    sqe->len = nr_vecs;
    sqe->off = static_cast<__u64>(offset);
}

// Sets the 32 bit flags of an op whatever the kernel header names that field.
inline void set_op_flags(struct io_uring_sqe* sqe, unsigned flags) {
    sqe->rw_flags = static_cast<decltype(sqe->rw_flags)>(flags);
}

// The result is a struct statx, which `statx_buffer` must keep until the
// request ends.
inline void prep_statx(struct io_uring_sqe* sqe, int dfd, const char* path,
                       int flags, unsigned mask, void* statx_buffer) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_STATX);
    sqe->fd = dfd;
    sqe->addr = reinterpret_cast<__u64>(path);
    sqe->len = mask;
    sqe->off = reinterpret_cast<__u64>(statx_buffer);
    set_op_flags(sqe, static_cast<unsigned>(flags));
}

inline void prep_unlinkat(struct io_uring_sqe* sqe, int dfd, const char* path,
                          int flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_UNLINKAT);
    sqe->fd = dfd;
    sqe->addr = reinterpret_cast<__u64>(path);
    set_op_flags(sqe, static_cast<unsigned>(flags));
}

inline void prep_renameat(struct io_uring_sqe* sqe, int olddfd,
                          const char* oldpath, int newdfd, const char* newpath,
                          unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_RENAMEAT);
    sqe->fd = olddfd;
    sqe->addr = reinterpret_cast<__u64>(oldpath);
    sqe->len = static_cast<__u32>(newdfd);
    sqe->off = reinterpret_cast<__u64>(newpath);
    set_op_flags(sqe, flags);
}

inline void prep_mkdirat(struct io_uring_sqe* sqe, int dfd, const char* path,
                         mode_t mode) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_MKDIRAT);
    sqe->fd = dfd;
    sqe->addr = reinterpret_cast<__u64>(path);
    sqe->len = static_cast<__u32>(mode);
}

inline void prep_fallocate(struct io_uring_sqe* sqe, int fd, int mode,
                           std::uint64_t offset, std::uint64_t len) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FALLOCATE);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = len;
    sqe->len = static_cast<__u32>(mode);
}

inline void prep_fadvise(struct io_uring_sqe* sqe, int fd, std::uint64_t offset,
                         std::uint32_t len, int advice) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FADVISE);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->len = len;
    set_op_flags(sqe, static_cast<unsigned>(advice));
}

inline void prep_sync_file_range(struct io_uring_sqe* sqe, int fd, unsigned len,
                                 std::uint64_t offset, int flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SYNC_FILE_RANGE);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->len = len;
    set_op_flags(sqe, static_cast<unsigned>(flags));
}

// An offset of -1 uses the file position. A pipe takes -1 only.
inline void prep_splice(struct io_uring_sqe* sqe, int fd_in,
                        std::int64_t off_in, int fd_out, std::int64_t off_out,
                        unsigned nbytes, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SPLICE);
    sqe->fd = fd_out;
    sqe->off = static_cast<__u64>(off_out);
    sqe->addr = static_cast<__u64>(off_in);
    sqe->len = nbytes;
    set_op_flags(sqe, flags);
    std::memcpy(reinterpret_cast<char*>(sqe) + SQE_SPLICE_FD_IN_OFFSET, &fd_in,
                sizeof(fd_in));
}

inline void prep_ftruncate(struct io_uring_sqe* sqe, int fd, std::int64_t len) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FTRUNCATE);
    sqe->fd = fd;
    sqe->off = static_cast<__u64>(len);
}

inline void set_sqe_u32(struct io_uring_sqe* sqe, std::size_t at,
                        std::uint32_t value) {
    std::memcpy(reinterpret_cast<char*>(sqe) + at, &value, sizeof(value));
}

inline void set_sqe_addr3(struct io_uring_sqe* sqe, std::uint64_t value) {
    std::memcpy(reinterpret_cast<char*>(sqe) + SQE_ADDR3_OFFSET, &value,
                sizeof(value));
}

// `how` is a struct open_how of `how_size` bytes.
inline void prep_openat2(struct io_uring_sqe* sqe, int dfd, const char* path,
                         const void* how, std::size_t how_size) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_OPENAT2);
    sqe->fd = dfd;
    sqe->addr = reinterpret_cast<__u64>(path);
    sqe->len = static_cast<__u32>(how_size);
    sqe->off = reinterpret_cast<__u64>(how);
}

inline void prep_linkat(struct io_uring_sqe* sqe, int olddfd,
                        const char* oldpath, int newdfd, const char* newpath,
                        int flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_LINKAT);
    sqe->fd = olddfd;
    sqe->addr = reinterpret_cast<__u64>(oldpath);
    sqe->len = static_cast<__u32>(newdfd);
    sqe->off = reinterpret_cast<__u64>(newpath);
    set_op_flags(sqe, static_cast<unsigned>(flags));
}

inline void prep_symlinkat(struct io_uring_sqe* sqe, const char* target,
                           int newdirfd, const char* linkpath) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SYMLINKAT);
    sqe->fd = newdirfd;
    sqe->addr = reinterpret_cast<__u64>(target);
    sqe->off = reinterpret_cast<__u64>(linkpath);
}

inline void prep_getxattr(struct io_uring_sqe* sqe, const char* name,
                          char* value, const char* path, unsigned len) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_GETXATTR);
    sqe->fd = 0;
    sqe->addr = reinterpret_cast<__u64>(name);
    sqe->len = len;
    sqe->off = reinterpret_cast<__u64>(value);
    set_sqe_addr3(sqe, reinterpret_cast<__u64>(path));
}

inline void prep_setxattr(struct io_uring_sqe* sqe, const char* name,
                          const char* value, const char* path, int flags,
                          unsigned len) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SETXATTR);
    sqe->fd = 0;
    sqe->addr = reinterpret_cast<__u64>(name);
    sqe->len = len;
    sqe->off = reinterpret_cast<__u64>(value);
    set_sqe_addr3(sqe, reinterpret_cast<__u64>(path));
    set_op_flags(sqe, static_cast<unsigned>(flags));
}

inline void prep_fgetxattr(struct io_uring_sqe* sqe, int fd, const char* name,
                           char* value, unsigned len) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FGETXATTR);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(name);
    sqe->len = len;
    sqe->off = reinterpret_cast<__u64>(value);
}

inline void prep_fsetxattr(struct io_uring_sqe* sqe, int fd, const char* name,
                           const char* value, int flags, unsigned len) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FSETXATTR);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(name);
    sqe->len = len;
    sqe->off = reinterpret_cast<__u64>(value);
    set_op_flags(sqe, static_cast<unsigned>(flags));
}

inline void prep_socket(struct io_uring_sqe* sqe, int domain, int type,
                        int protocol, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SOCKET);
    sqe->fd = domain;
    sqe->len = static_cast<__u32>(protocol);
    sqe->off = static_cast<__u64>(type);
    set_op_flags(sqe, flags);
}

inline void prep_connect(struct io_uring_sqe* sqe, int fd,
                         const struct sockaddr* addr, socklen_t addrlen) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_CONNECT);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(addr);
    sqe->off = static_cast<__u64>(addrlen);
}

inline void prep_bind(struct io_uring_sqe* sqe, int fd,
                      const struct sockaddr* addr, socklen_t addrlen) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_BIND);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(addr);
    sqe->off = static_cast<__u64>(addrlen);
}

inline void prep_listen(struct io_uring_sqe* sqe, int fd, int backlog) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_LISTEN);
    sqe->fd = fd;
    sqe->len = static_cast<__u32>(backlog);
}

inline void prep_shutdown(struct io_uring_sqe* sqe, int fd, int how) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SHUTDOWN);
    sqe->fd = fd;
    sqe->len = static_cast<__u32>(how);
}

inline void prep_sendmsg(struct io_uring_sqe* sqe, int fd,
                         const struct msghdr* msg, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SENDMSG);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(msg);
    sqe->len = 1;
    sqe->msg_flags = flags;
}

inline void prep_recvmsg(struct io_uring_sqe* sqe, int fd, struct msghdr* msg,
                         unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_RECVMSG);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(msg);
    sqe->len = 1;
    sqe->msg_flags = flags;
}

inline void prep_tee(struct io_uring_sqe* sqe, int fd_in, int fd_out,
                     unsigned nbytes, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_TEE);
    sqe->fd = fd_out;
    sqe->len = nbytes;
    set_op_flags(sqe, flags);
    set_sqe_u32(sqe, SQE_SPLICE_FD_IN_OFFSET,
                static_cast<std::uint32_t>(fd_in));
}

inline void prep_pipe(struct io_uring_sqe* sqe, int* fds, int pipe_flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_PIPE);
    sqe->fd = 0;
    sqe->addr = reinterpret_cast<__u64>(fds);
    set_op_flags(sqe, static_cast<unsigned>(pipe_flags));
}

inline void prep_madvise(struct io_uring_sqe* sqe, void* addr, unsigned length,
                         int advice) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_MADVISE);
    sqe->fd = -1;
    sqe->addr = reinterpret_cast<__u64>(addr);
    sqe->len = length;
    set_op_flags(sqe, static_cast<unsigned>(advice));
}

// `infop` is a siginfo_t that must live until the request ends. The options go
// where file_index is, as the kernel reads them there.
inline void prep_waitid(struct io_uring_sqe* sqe, int idtype, int id,
                        void* infop, int options, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_WAITID);
    sqe->fd = id;
    sqe->len = static_cast<__u32>(idtype);
    sqe->addr2 = reinterpret_cast<__u64>(infop);
    set_op_flags(sqe, flags);
    set_sqe_u32(sqe, SQE_FILE_INDEX_OFFSET,
                static_cast<std::uint32_t>(options));
}

// `futex_flags` name the size and the sharing of the word, `flags` are the
// SQE's own and stay 0 today.
inline void prep_futex_wait(struct io_uring_sqe* sqe,
                            const std::uint32_t* futex, std::uint64_t val,
                            std::uint64_t mask, std::uint32_t futex_flags,
                            unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FUTEX_WAIT);
    sqe->fd = static_cast<__s32>(futex_flags);
    sqe->addr = reinterpret_cast<__u64>(futex);
    sqe->off = val;
    set_op_flags(sqe, flags);
    set_sqe_addr3(sqe, mask);
}

inline void prep_futex_wake(struct io_uring_sqe* sqe,
                            const std::uint32_t* futex, std::uint64_t val,
                            std::uint64_t mask, std::uint32_t futex_flags,
                            unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FUTEX_WAKE);
    sqe->fd = static_cast<__s32>(futex_flags);
    sqe->addr = reinterpret_cast<__u64>(futex);
    sqe->off = val;
    set_op_flags(sqe, flags);
    set_sqe_addr3(sqe, mask);
}

// `futexv` is an array of `nr_futex` struct futex_waitv.
inline void prep_futex_waitv(struct io_uring_sqe* sqe, const void* futexv,
                             unsigned nr_futex, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FUTEX_WAITV);
    sqe->fd = 0;
    sqe->addr = reinterpret_cast<__u64>(futexv);
    sqe->len = nr_futex;
    set_op_flags(sqe, flags);
}

inline void prep_epoll_ctl(struct io_uring_sqe* sqe, int epfd, int fd, int op,
                           const void* ev) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_EPOLL_CTL);
    sqe->fd = epfd;
    sqe->addr = reinterpret_cast<__u64>(ev);
    sqe->len = static_cast<__u32>(op);
    sqe->off = static_cast<__u32>(fd);
}

// There is no timeout: the wait ends with an event or a cancel.
inline void prep_epoll_wait(struct io_uring_sqe* sqe, int fd, void* events,
                            int maxevents, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_EPOLL_WAIT);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(events);
    sqe->len = static_cast<__u32>(maxevents);
    set_op_flags(sqe, flags);
}

inline void prep_poll_add(struct io_uring_sqe* sqe, int fd,
                          unsigned poll_mask) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_POLL_ADD);
    sqe->fd = fd;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    poll_mask = (poll_mask << 16) | (poll_mask >> 16);
#endif
    set_op_flags(sqe, poll_mask);
}

inline void sqe_set_data(struct io_uring_sqe* sqe, void* data) {
    sqe->user_data = reinterpret_cast<__u64>(data);
}

}  // namespace dftracer::utils::runtime::io::uring

#endif  // DFTRACER_UTILS_HAVE_IO_URING
#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_URING_WRAPPER_H
