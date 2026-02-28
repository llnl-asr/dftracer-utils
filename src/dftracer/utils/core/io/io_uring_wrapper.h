#pragma once
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <linux/io_uring.h>
// linux/io_uring.h may pull in kernel headers that define BLOCK_SIZE
// as a macro, which collides with concurrentqueue. Undefine it.
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::io::uring {

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

/// Owns the ring fd and mmap'd memory. Not thread-safe -- caller
/// must synchronize SQE submission (submit_mutex_ in the backend).
class Ring {
    int ring_fd_ = -1;

    // SQ ring (submission queue)
    void* sq_ring_ptr_ = nullptr;
    std::size_t sq_ring_size_ = 0;
    unsigned* sq_head_ = nullptr;
    unsigned* sq_tail_ = nullptr;
    unsigned* sq_ring_mask_ = nullptr;
    unsigned* sq_ring_entries_ = nullptr;
    unsigned* sq_array_ = nullptr;

    // CQ ring (completion queue)
    void* cq_ring_ptr_ = nullptr;
    std::size_t cq_ring_size_ = 0;
    unsigned* cq_head_ = nullptr;
    unsigned* cq_tail_ = nullptr;
    unsigned* cq_ring_mask_ = nullptr;
    unsigned* cq_ring_entries_ = nullptr;
    struct io_uring_cqe* cqes_ = nullptr;

    // SQE array
    struct io_uring_sqe* sqes_ = nullptr;
    std::size_t sqes_size_ = 0;

    // Batched submission: count of SQEs prepared but not yet
    // submitted to the kernel via io_uring_enter.
    unsigned pending_count_ = 0;

   public:
    Ring() = default;
    ~Ring() { destroy(); }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    /// Initialize the ring. Returns true on success, false if kernel
    /// doesn't support io_uring (ENOSYS) or other setup failure.
    bool init(unsigned entries);

    /// Tear down: munmap + close fd.
    void destroy();

    bool is_valid() const { return ring_fd_ >= 0; }
    int fd() const { return ring_fd_; }

    /// Get next available SQE slot. Returns nullptr if SQ is full.
    struct io_uring_sqe* get_sqe();

    /// Increment pending count after preparing an SQE.
    /// Call after get_sqe() + prep_*() to record that this SQE
    /// should be submitted on the next flush().
    void mark_pending() { ++pending_count_; }

    /// Number of SQEs prepared but not yet submitted.
    unsigned pending_count() const { return pending_count_; }

    /// Submit all pending SQEs to the kernel in a single
    /// io_uring_enter syscall. Returns the number of SQEs
    /// submitted, or negative errno on failure.
    int flush();

    /// Submit exactly one SQE (legacy path, used by stop() NOP).
    int submit();

    /// Non-blocking peek at next CQE. Returns nullptr if CQ empty.
    struct io_uring_cqe* peek_cqe();

    /// Blocking wait for at least one CQE.
    /// Returns 0 on success, negative errno on failure.
    int wait_cqe(struct io_uring_cqe** cqe_out);

    /// Mark a CQE as consumed (advance CQ head).
    void cqe_seen(struct io_uring_cqe* cqe);
};

// ============================================================================
// SQE preparation helpers (inline, match liburing API names)
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

inline void sqe_set_data(struct io_uring_sqe* sqe, void* data) {
    sqe->user_data = reinterpret_cast<__u64>(data);
}

inline void* cqe_get_data(struct io_uring_cqe* cqe) {
    return reinterpret_cast<void*>(cqe->user_data);
}

}  // namespace dftracer::utils::io::uring

#endif  // DFTRACER_UTILS_HAVE_IO_URING
