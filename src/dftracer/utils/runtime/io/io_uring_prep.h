// SPDX-License-Identifier: MIT
// The opcode numbers and the SQE layouts below follow liburing,
// https://github.com/axboe/liburing (MIT), Copyright 2020 Jens Axboe, files
// src/include/liburing.h and src/include/liburing/io_uring.h.
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_URING_PREP_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_URING_PREP_H
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/runtime/io/io_uring_wrapper.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef IOSQE_FIXED_FILE
#define IOSQE_FIXED_FILE (1U << 0)
#endif
#ifndef IOSQE_IO_LINK
#define IOSQE_IO_LINK (1U << 2)
#endif
#ifndef IOSQE_ASYNC
#define IOSQE_ASYNC (1U << 4)
#endif
#ifndef IOSQE_BUFFER_SELECT
#define IOSQE_BUFFER_SELECT (1U << 5)
#endif
#ifndef IORING_FILE_INDEX_ALLOC
#define IORING_FILE_INDEX_ALLOC (~0U)
#endif
#ifndef IORING_RECVSEND_POLL_FIRST
#define IORING_RECVSEND_POLL_FIRST (1U << 0)
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_RECVSEND_FIXED_BUF
#define IORING_RECVSEND_FIXED_BUF (1U << 2)
#endif
#ifndef IORING_SEND_ZC_REPORT_USAGE
#define IORING_SEND_ZC_REPORT_USAGE (1U << 3)
#endif
#ifndef IORING_CQE_F_BUFFER
#define IORING_CQE_F_BUFFER (1U << 0)
#endif
#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE (1U << 1)
#endif
#ifndef IORING_CQE_F_SOCK_NONEMPTY
#define IORING_CQE_F_SOCK_NONEMPTY (1U << 2)
#endif
#ifndef IORING_CQE_F_NOTIF
#define IORING_CQE_F_NOTIF (1U << 3)
#endif
#ifndef IORING_CQE_F_BUF_MORE
#define IORING_CQE_F_BUF_MORE (1U << 4)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif
#ifndef IORING_MSG_RING_CQE_SKIP
#define IORING_MSG_RING_CQE_SKIP (1U << 0)
#endif
#ifndef IORING_MSG_RING_FLAGS_PASS
#define IORING_MSG_RING_FLAGS_PASS (1U << 1)
#endif
#ifndef IORING_FIXED_FD_NO_CLOEXEC
#define IORING_FIXED_FD_NO_CLOEXEC (1U << 0)
#endif
#ifndef IORING_URING_CMD_FIXED
#define IORING_URING_CMD_FIXED (1U << 0)
#endif
#ifndef IORING_NOP_CQE32
#define IORING_NOP_CQE32 (1U << 5)
#endif
#ifndef IORING_TIMEOUT_ABS
#define IORING_TIMEOUT_ABS (1U << 0)
#endif
#ifndef IORING_TIMEOUT_UPDATE
#define IORING_TIMEOUT_UPDATE (1U << 1)
#endif
#ifndef IORING_TIMEOUT_BOOTTIME
#define IORING_TIMEOUT_BOOTTIME (1U << 2)
#endif
#ifndef IORING_TIMEOUT_REALTIME
#define IORING_TIMEOUT_REALTIME (1U << 3)
#endif
#ifndef IORING_LINK_TIMEOUT_UPDATE
#define IORING_LINK_TIMEOUT_UPDATE (1U << 4)
#endif
#ifndef IORING_TIMEOUT_ETIME_SUCCESS
#define IORING_TIMEOUT_ETIME_SUCCESS (1U << 5)
#endif

namespace dftracer::utils::runtime::io::uring {

// The opcodes are enum values in the kernel header, so a macro test cannot see
// whether an older header lacks them.
constexpr unsigned OP_READ_FIXED = 4;
constexpr unsigned OP_WRITE_FIXED = 5;
constexpr unsigned OP_POLL_REMOVE = 7;
constexpr unsigned OP_TIMEOUT_REMOVE = 12;
constexpr unsigned OP_LINK_TIMEOUT = 15;
constexpr unsigned OP_FILES_UPDATE = 20;
constexpr unsigned OP_PROVIDE_BUFFERS = 31;
constexpr unsigned OP_REMOVE_BUFFERS = 32;
constexpr unsigned OP_MSG_RING = 40;
constexpr unsigned OP_URING_CMD = 46;
constexpr unsigned OP_SEND_ZC = 47;
constexpr unsigned OP_SENDMSG_ZC = 48;
constexpr unsigned OP_READ_MULTISHOT = 49;
constexpr unsigned OP_FIXED_FD_INSTALL = 54;
constexpr unsigned OP_RECV_ZC = 58;
constexpr unsigned OP_READV_FIXED = 60;
constexpr unsigned OP_WRITEV_FIXED = 61;
constexpr unsigned OP_NOP128 = 63;
constexpr unsigned OP_URING_CMD128 = 64;

// io_uring_msg_ring_flags, stored in sqe->addr of a MSG_RING.
constexpr std::uint64_t MSG_DATA = 0;
constexpr std::uint64_t MSG_SEND_FD = 1;

// io_uring_socket_op, the cmd_op of a URING_CMD on a socket.
constexpr std::uint32_t SOCKET_OP_SIOCINQ = 0;
constexpr std::uint32_t SOCKET_OP_SIOCOUTQ = 1;
constexpr std::uint32_t SOCKET_OP_GETSOCKOPT = 2;
constexpr std::uint32_t SOCKET_OP_SETSOCKOPT = 3;
constexpr std::uint32_t SOCKET_OP_TX_TIMESTAMP = 4;
constexpr std::uint32_t SOCKET_OP_GETSOCKNAME = 5;

// The bytes of an SQE that older headers lack or name otherwise.
constexpr std::size_t SQE_CMD_OP_OFFSET = 8;
constexpr std::size_t SQE_LEVEL_OFFSET = 16;
constexpr std::size_t SQE_OPTNAME_OFFSET = 20;
constexpr std::size_t SQE_BUF_INDEX_OFFSET = 40;
constexpr std::size_t SQE_PERSONALITY_OFFSET = 42;
constexpr std::size_t SQE_OPTLEN_OFFSET = 44;
constexpr std::size_t SQE_CMD_OFFSET = 48;

template <typename T>
inline void put_sqe_field(struct io_uring_sqe* sqe, std::size_t at, T value) {
    std::memcpy(reinterpret_cast<char*>(sqe) + at, &value, sizeof(value));
}

inline void set_buf_index(struct io_uring_sqe* sqe, unsigned index) {
    put_sqe_field(sqe, SQE_BUF_INDEX_OFFSET, static_cast<std::uint16_t>(index));
}

// The buffer group of a request with IOSQE_BUFFER_SELECT. It shares the bytes
// of the fixed buffer index.
inline void set_buf_group(struct io_uring_sqe* sqe, unsigned group) {
    set_buf_index(sqe, group);
}

inline void set_personality(struct io_uring_sqe* sqe, unsigned id) {
    put_sqe_field(sqe, SQE_PERSONALITY_OFFSET, static_cast<std::uint16_t>(id));
}

// Aims an op that makes a descriptor at the fixed file `index`. Pass
// IORING_FILE_INDEX_ALLOC to let the kernel pick, which it gives in cqe->res.
inline void set_target_fixed_file(struct io_uring_sqe* sqe, unsigned index) {
    put_sqe_field(sqe, SQE_FILE_INDEX_OFFSET,
                  index == IORING_FILE_INDEX_ALLOC
                      ? static_cast<std::uint32_t>(IORING_FILE_INDEX_ALLOC)
                      : static_cast<std::uint32_t>(index + 1));
}

inline bool cqe_has_buffer(const struct io_uring_cqe* cqe) {
    return (cqe->flags & IORING_CQE_F_BUFFER) != 0;
}

inline unsigned cqe_buffer_id(const struct io_uring_cqe* cqe) {
    return cqe->flags >> IORING_CQE_BUFFER_SHIFT;
}

// ============================================================================
// Fixed buffers
// ============================================================================

// `buf` must lie in the registered buffer at `buf_index`.
inline void prep_read_fixed(struct io_uring_sqe* sqe, int fd, void* buf,
                            unsigned nbytes, std::uint64_t offset,
                            unsigned buf_index) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_READ_FIXED);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = nbytes;
    set_buf_index(sqe, buf_index);
}

inline void prep_write_fixed(struct io_uring_sqe* sqe, int fd, const void* buf,
                             unsigned nbytes, std::uint64_t offset,
                             unsigned buf_index) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_WRITE_FIXED);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = nbytes;
    set_buf_index(sqe, buf_index);
}

// The vectors must lie in the registered buffer at `buf_index`. `rw_flags` are
// the RWF_* bits.
inline void prep_readv_fixed(struct io_uring_sqe* sqe, int fd,
                             const struct iovec* iov, unsigned nr_vecs,
                             std::uint64_t offset, unsigned rw_flags,
                             unsigned buf_index) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_READV_FIXED);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = reinterpret_cast<__u64>(iov);
    sqe->len = nr_vecs;
    set_op_flags(sqe, rw_flags);
    set_buf_index(sqe, buf_index);
}

inline void prep_writev_fixed(struct io_uring_sqe* sqe, int fd,
                              const struct iovec* iov, unsigned nr_vecs,
                              std::uint64_t offset, unsigned rw_flags,
                              unsigned buf_index) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_WRITEV_FIXED);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = reinterpret_cast<__u64>(iov);
    sqe->len = nr_vecs;
    set_op_flags(sqe, rw_flags);
    set_buf_index(sqe, buf_index);
}

// ============================================================================
// Fixed files
// ============================================================================

// Puts `fds[0..nr_fds)` at the fixed file slots from `offset`. The result is
// the count updated. `fds` must live until the request ends.
inline void prep_files_update(struct io_uring_sqe* sqe, const int* fds,
                              unsigned nr_fds, unsigned offset) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FILES_UPDATE);
    sqe->fd = -1;
    sqe->addr = reinterpret_cast<__u64>(fds);
    sqe->len = nr_fds;
    sqe->off = offset;
}

// `fixed_index` is a registered file. The result is a new ordinary descriptor.
inline void prep_fixed_fd_install(struct io_uring_sqe* sqe, int fixed_index,
                                  unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_FIXED_FD_INSTALL);
    sqe->fd = fixed_index;
    sqe->flags = static_cast<__u8>(IOSQE_FIXED_FILE);
    set_op_flags(sqe, flags);
}

// ============================================================================
// Provided buffers
// ============================================================================

// Adds `nr` buffers of `len` bytes from `addr`, with the ids from `bid`, to the
// group `bgid`. A request that uses them sets IOSQE_BUFFER_SELECT and
// set_buf_group().
inline void prep_provide_buffers(struct io_uring_sqe* sqe, void* addr,
                                 unsigned len, unsigned nr, unsigned bgid,
                                 unsigned bid) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_PROVIDE_BUFFERS);
    sqe->fd = static_cast<__s32>(nr);
    sqe->addr = reinterpret_cast<__u64>(addr);
    sqe->len = len;
    sqe->off = bid;
    set_buf_group(sqe, bgid);
}

// The result is the count removed.
inline void prep_remove_buffers(struct io_uring_sqe* sqe, unsigned nr,
                                unsigned bgid) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_REMOVE_BUFFERS);
    sqe->fd = static_cast<__s32>(nr);
    set_buf_group(sqe, bgid);
}

// Reads into a buffer of the group each time data comes, while the CQE has
// IORING_CQE_F_MORE. `nbytes` of 0 takes the buffer's size.
inline void prep_read_multishot(struct io_uring_sqe* sqe, int fd,
                                unsigned nbytes, std::uint64_t offset,
                                unsigned buf_group) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_READ_MULTISHOT);
    sqe->fd = fd;
    sqe->off = offset;
    sqe->len = nbytes;
    sqe->flags = static_cast<__u8>(IOSQE_BUFFER_SELECT);
    set_buf_group(sqe, buf_group);
}

// ============================================================================
// Sockets
// ============================================================================

// Two CQEs come: the send, with IORING_CQE_F_MORE, then a notification with
// IORING_CQE_F_NOTIF. `buf` must live until the notification.
inline void prep_send_zc(struct io_uring_sqe* sqe, int fd, const void* buf,
                         unsigned len, int flags, unsigned zc_flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SEND_ZC);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = len;
    sqe->msg_flags = static_cast<__u32>(flags);
    sqe->ioprio = static_cast<__u16>(zc_flags);
}

// As prep_send_zc, with `buf` inside the registered buffer at `buf_index`.
inline void prep_send_zc_fixed(struct io_uring_sqe* sqe, int fd,
                               const void* buf, unsigned len, int flags,
                               unsigned zc_flags, unsigned buf_index) {
    prep_send_zc(sqe, fd, buf, len, flags,
                 zc_flags | IORING_RECVSEND_FIXED_BUF);
    set_buf_index(sqe, buf_index);
}

inline void prep_sendmsg_zc(struct io_uring_sqe* sqe, int fd,
                            const struct msghdr* msg, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_SENDMSG_ZC);
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(msg);
    sqe->len = 1;
    sqe->msg_flags = flags;
}

inline void prep_sendmsg_zc_fixed(struct io_uring_sqe* sqe, int fd,
                                  const struct msghdr* msg, unsigned flags,
                                  unsigned buf_index) {
    prep_sendmsg_zc(sqe, fd, msg, flags);
    sqe->ioprio = static_cast<__u16>(IORING_RECVSEND_FIXED_BUF);
    set_buf_index(sqe, buf_index);
}

// Zero copy receive into the area of the registered interface queue
// `zcrx_ifq_idx`. It is a multishot request.
inline void prep_recv_zc(struct io_uring_sqe* sqe, int fd, unsigned len,
                         unsigned zcrx_ifq_idx) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_RECV_ZC);
    sqe->fd = fd;
    sqe->len = len;
    sqe->ioprio = static_cast<__u16>(IORING_RECV_MULTISHOT);
    put_sqe_field(sqe, SQE_FILE_INDEX_OFFSET,
                  static_cast<std::uint32_t>(zcrx_ifq_idx));
}

// ============================================================================
// Timeouts, polls and links
// ============================================================================

// Cancels the timeout whose user_data is `target`, which then ends with
// -ECANCELED.
inline void prep_timeout_remove(struct io_uring_sqe* sqe, std::uint64_t target,
                                unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_TIMEOUT_REMOVE);
    sqe->fd = -1;
    sqe->addr = target;
    set_op_flags(sqe, flags);
}

// Gives the timeout whose user_data is `target` a new time. `ts` must live
// until the kernel takes the request.
inline void prep_timeout_update(struct io_uring_sqe* sqe,
                                const KernelTimespec* ts, std::uint64_t target,
                                unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_TIMEOUT_REMOVE);
    sqe->fd = -1;
    sqe->addr = target;
    sqe->off = reinterpret_cast<__u64>(ts);
    set_op_flags(sqe, flags | IORING_TIMEOUT_UPDATE);
}

// Bounds the request before it, which must have IOSQE_IO_LINK. `ts` must live
// until the kernel takes the request.
inline void prep_link_timeout(struct io_uring_sqe* sqe,
                              const KernelTimespec* ts, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_LINK_TIMEOUT);
    sqe->fd = -1;
    sqe->addr = reinterpret_cast<__u64>(ts);
    sqe->len = 1;
    set_op_flags(sqe, flags);
}

// Cancels the poll whose user_data is `target`, which then ends with
// -ECANCELED.
inline void prep_poll_remove(struct io_uring_sqe* sqe, std::uint64_t target) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_POLL_REMOVE);
    sqe->fd = -1;
    sqe->addr = target;
}

// ============================================================================
// Rings talking to each other
// ============================================================================

// Posts a CQE with user_data `data` and res `len` to the ring `ring_fd`.
inline void prep_msg_ring(struct io_uring_sqe* sqe, int ring_fd, unsigned len,
                          std::uint64_t data, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_MSG_RING);
    sqe->fd = ring_fd;
    sqe->off = data;
    sqe->addr = MSG_DATA;
    sqe->len = len;
    set_op_flags(sqe, flags);
}

// As prep_msg_ring, with the CQE flags `cqe_flags`.
inline void prep_msg_ring_cqe_flags(struct io_uring_sqe* sqe, int ring_fd,
                                    unsigned len, std::uint64_t data,
                                    unsigned flags, unsigned cqe_flags) {
    prep_msg_ring(sqe, ring_fd, len, data, flags | IORING_MSG_RING_FLAGS_PASS);
    put_sqe_field(sqe, SQE_FILE_INDEX_OFFSET,
                  static_cast<std::uint32_t>(cqe_flags));
}

// Sends the registered file `source_fixed` to the fixed file slot
// `target_fixed` of the ring `ring_fd`, or to a slot it picks with
// IORING_FILE_INDEX_ALLOC.
inline void prep_msg_ring_fd(struct io_uring_sqe* sqe, int ring_fd,
                             int source_fixed, unsigned target_fixed,
                             std::uint64_t data, unsigned flags) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_MSG_RING);
    sqe->fd = ring_fd;
    sqe->off = data;
    sqe->addr = MSG_SEND_FD;
    set_op_flags(sqe, flags);
    set_sqe_addr3(sqe, static_cast<std::uint64_t>(source_fixed));
    set_target_fixed_file(sqe, target_fixed);
}

// ============================================================================
// Commands
// ============================================================================

// The command's own bytes start here: 16 of them in a 64 byte entry, 80 in a
// 128 byte one.
inline std::uint8_t* uring_cmd_data(struct io_uring_sqe* sqe) {
    return reinterpret_cast<std::uint8_t*>(sqe) + SQE_CMD_OFFSET;
}

inline void prep_uring_cmd_op(struct io_uring_sqe* sqe, unsigned opcode,
                              std::uint32_t cmd_op, int fd) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(opcode);
    sqe->fd = fd;
    put_sqe_field(sqe, SQE_CMD_OP_OFFSET, cmd_op);
}

inline void prep_uring_cmd(struct io_uring_sqe* sqe, std::uint32_t cmd_op,
                           int fd) {
    prep_uring_cmd_op(sqe, OP_URING_CMD, cmd_op, fd);
}

// For a 128 byte entry from Ring::get_sqe128. It clears the first 64 bytes
// only; the caller clears the command bytes it does not write.
inline void prep_uring_cmd128(struct io_uring_sqe* sqe, std::uint32_t cmd_op,
                              int fd) {
    prep_uring_cmd_op(sqe, OP_URING_CMD128, cmd_op, fd);
}

// A command on a socket: SOCKET_OP_*.
inline void prep_cmd_sock(struct io_uring_sqe* sqe, std::uint32_t cmd_op,
                          int fd, int level, int optname, void* optval,
                          int optlen) {
    prep_uring_cmd(sqe, cmd_op, fd);
    put_sqe_field(sqe, SQE_CMD_OFFSET, reinterpret_cast<std::uint64_t>(optval));
    put_sqe_field(sqe, SQE_OPTNAME_OFFSET, static_cast<std::uint32_t>(optname));
    put_sqe_field(sqe, SQE_OPTLEN_OFFSET, static_cast<std::uint32_t>(optlen));
    put_sqe_field(sqe, SQE_LEVEL_OFFSET, static_cast<std::uint32_t>(level));
}

// A NOP in a 128 byte entry from Ring::get_sqe128. It clears the first 64 bytes
// only.
inline void prep_nop128(struct io_uring_sqe* sqe) {
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = static_cast<__u8>(OP_NOP128);
    sqe->fd = -1;
}

}  // namespace dftracer::utils::runtime::io::uring

#endif  // DFTRACER_UTILS_HAVE_IO_URING
#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_URING_PREP_H
