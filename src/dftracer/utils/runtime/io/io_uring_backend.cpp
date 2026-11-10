// SPDX-License-Identifier: MIT
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/runtime/io/io_uring_backend.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace dftracer::utils::runtime::io {

// ============================================================================
// Ring implementation
// ============================================================================

namespace uring {

bool Ring::init(unsigned entries, unsigned setup_flags, unsigned sq_idle_ms) {
    return init(entries,
                RingOptions{.flags = setup_flags, .sq_idle_ms = sq_idle_ms});
}

bool Ring::init(unsigned entries, const RingOptions& options) {
    const unsigned setup_flags = options.flags;
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = setup_flags;
    params.sq_thread_idle = options.sq_idle_ms;
    params.cq_entries = options.cq_entries;
    params.sq_thread_cpu = options.sq_cpu;
    if (options.attach_fd >= 0) {
        params.wq_fd = static_cast<unsigned>(options.attach_fd);
    }
    is_sq_polled_ = (setup_flags & IORING_SETUP_SQPOLL) != 0;
    deferred_ = (setup_flags & IORING_SETUP_DEFER_TASKRUN) != 0;
    is_iopoll_ = (setup_flags & IORING_SETUP_IOPOLL) != 0;
    sqe_shift_ = (setup_flags & IORING_SETUP_SQE128) != 0 ? 1U : 0U;
    cqe_shift_ = (setup_flags & IORING_SETUP_CQE32) != 0 ? 1U : 0U;
    is_sqe_mixed_ = (setup_flags & IORING_SETUP_SQE_MIXED) != 0;
    is_cqe_mixed_ = (setup_flags & IORING_SETUP_CQE_MIXED) != 0;
    is_sq_rewound_ = (setup_flags & IORING_SETUP_SQ_REWIND) != 0;
    is_fd_registered_ = (setup_flags & IORING_SETUP_REGISTERED_FD_ONLY) != 0;
    enter_flags_ = is_fd_registered_ ? IORING_ENTER_REGISTERED_RING : 0U;

    const bool has_own_memory = (setup_flags & IORING_SETUP_NO_MMAP) != 0;
    if (has_own_memory && !map_own_memory(entries, options, params)) {
        return false;
    }

    ring_fd_ = sys_io_uring_setup(entries, &params);
    if (ring_fd_ < 0) {
        if (has_own_memory) unmap_own_memory();
        return false;
    }
    enter_fd_ = ring_fd_;
    features_ = params.features;
    cq_entries_ = params.cq_entries;
    pending_ = 0;

    const bool has_sq_array = (setup_flags & IORING_SETUP_NO_SQARRAY) == 0;
    sq_ring_size_ =
        params.sq_off.array +
        (has_sq_array ? params.sq_entries * sizeof(unsigned) : std::size_t{0});
    cq_ring_size_ =
        params.cq_off.cqes + (std::size_t{params.cq_entries} << cqe_shift_) *
                                 sizeof(struct io_uring_cqe);
    // The kernel keeps both rings in one region and either offset maps it.
    const bool is_single_mapping =
        (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
    if (is_single_mapping) {
        sq_ring_size_ = std::max(sq_ring_size_, cq_ring_size_);
        cq_ring_size_ = sq_ring_size_;
    }

    // Map SQ ring
    if (has_own_memory) {
        sq_ring_size_ = own_ring_size_;
        cq_ring_size_ = own_ring_size_;
    } else {
        sq_ring_ptr_ =
            ::mmap(nullptr, sq_ring_size_, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_ring_ptr_ == MAP_FAILED) {
            ::close(ring_fd_);
            ring_fd_ = -1;
            return false;
        }
    }

    auto* sq_base = static_cast<char*>(sq_ring_ptr_);
    sq_head_ = reinterpret_cast<unsigned*>(sq_base + params.sq_off.head);
    sq_tail_ = reinterpret_cast<unsigned*>(sq_base + params.sq_off.tail);
    sq_ring_mask_ =
        reinterpret_cast<unsigned*>(sq_base + params.sq_off.ring_mask);
    sq_ring_entries_ =
        reinterpret_cast<unsigned*>(sq_base + params.sq_off.ring_entries);
    if (has_sq_array) {
        auto* const sq_array =
            reinterpret_cast<unsigned*>(sq_base + params.sq_off.array);
        for (unsigned i = 0; i < params.sq_entries; ++i) sq_array[i] = i;
    }
    sqe_tail_ = 0;
    sq_flags_ = reinterpret_cast<unsigned*>(sq_base + params.sq_off.flags);

    // Map SQEs
    if (!has_own_memory) {
        sqes_size_ = (std::size_t{params.sq_entries} << sqe_shift_) *
                     sizeof(struct io_uring_sqe);
        sqes_ = static_cast<struct io_uring_sqe*>(
            ::mmap(nullptr, sqes_size_, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES));
    }
    if (sqes_ == MAP_FAILED) {
        ::munmap(sq_ring_ptr_, sq_ring_size_);
        ::close(ring_fd_);
        sq_ring_ptr_ = nullptr;
        sqes_ = nullptr;
        ring_fd_ = -1;
        return false;
    }

    // Map CQ ring
    cq_ring_ptr_ =
        is_single_mapping || has_own_memory
            ? sq_ring_ptr_
            : ::mmap(nullptr, cq_ring_size_, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
    if (cq_ring_ptr_ == MAP_FAILED) {
        ::munmap(sqes_, sqes_size_);
        ::munmap(sq_ring_ptr_, sq_ring_size_);
        ::close(ring_fd_);
        cq_ring_ptr_ = nullptr;
        sqes_ = nullptr;
        sq_ring_ptr_ = nullptr;
        ring_fd_ = -1;
        return false;
    }

    cq_seen_ = 0;
    cq_committed_ = 0;
    cq_visible_tail_ = 0;
    auto* cq_base = static_cast<char*>(cq_ring_ptr_);
    cq_head_ = reinterpret_cast<unsigned*>(cq_base + params.cq_off.head);
    cq_tail_ = reinterpret_cast<unsigned*>(cq_base + params.cq_off.tail);
    cq_ring_mask_ =
        reinterpret_cast<unsigned*>(cq_base + params.cq_off.ring_mask);
    cqes_ =
        reinterpret_cast<struct io_uring_cqe*>(cq_base + params.cq_off.cqes);

    return true;
}

bool Ring::map_own_memory(unsigned entries, const RingOptions& options,
                          struct io_uring_params& params) {
    // Layout after liburing's io_uring_alloc_huge. The kernel wants each region
    // inside one folio, so a region past a page needs a huge page.
    constexpr std::size_t RING_HEADER_BYTES = 64;
    constexpr std::size_t HUGE_PAGE_BYTES = std::size_t{2} << 20;
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    std::size_t sq_entries = 1;
    while (sq_entries < entries) sq_entries <<= 1;
    const std::size_t cq_entries = (options.flags & IORING_SETUP_CQSIZE) != 0
                                       ? options.cq_entries
                                       : 2 * sq_entries;
    const std::size_t sqes_bytes =
        (sq_entries << sqe_shift_) * sizeof(struct io_uring_sqe);
    std::size_t ring_bytes =
        RING_HEADER_BYTES +
        (cq_entries << cqe_shift_) * sizeof(struct io_uring_cqe);
    if ((options.flags & IORING_SETUP_NO_SQARRAY) == 0) {
        ring_bytes += sq_entries * sizeof(unsigned);
    }
    const auto map_region = [&](std::size_t bytes, std::size_t& mapped) {
        const bool is_huge = bytes > page;
        mapped = is_huge ? HUGE_PAGE_BYTES : page;
        if (bytes > mapped) return MAP_FAILED;
        return ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | (is_huge ? MAP_HUGETLB : 0),
                      -1, 0);
    };
    void* const sqes = map_region(sqes_bytes, sqes_size_);
    if (sqes == MAP_FAILED) return false;
    void* const rings = map_region(ring_bytes, own_ring_size_);
    if (rings == MAP_FAILED) {
        ::munmap(sqes, sqes_size_);
        return false;
    }
    sqes_ = static_cast<struct io_uring_sqe*>(sqes);
    sq_ring_ptr_ = rings;
    // user_addr sits in the last word of each block. An older header names it
    // resv2 or resv, so it is written by offset.
    constexpr std::size_t USER_ADDR_OFFSET = 32;
    const std::uint64_t sqes_addr = reinterpret_cast<std::uint64_t>(sqes);
    const std::uint64_t rings_addr = reinterpret_cast<std::uint64_t>(rings);
    std::memcpy(reinterpret_cast<char*>(&params.sq_off) + USER_ADDR_OFFSET,
                &sqes_addr, sizeof(sqes_addr));
    std::memcpy(reinterpret_cast<char*>(&params.cq_off) + USER_ADDR_OFFSET,
                &rings_addr, sizeof(rings_addr));
    return true;
}

void Ring::unmap_own_memory() {
    if (sqes_ != nullptr) ::munmap(sqes_, sqes_size_);
    if (sq_ring_ptr_ != nullptr) ::munmap(sq_ring_ptr_, own_ring_size_);
    sqes_ = nullptr;
    sq_ring_ptr_ = nullptr;
}

void Ring::enter_through_index(unsigned index) {
    enter_fd_ = static_cast<int>(index);
    enter_flags_ |= IORING_ENTER_REGISTERED_RING;
}

int Ring::register_call(unsigned opcode, void* arg, unsigned count) {
    return sys_io_uring_register(
        ring_fd_,
        is_fd_registered_ ? opcode | REGISTER_USE_REGISTERED_RING : opcode, arg,
        count);
}

void Ring::destroy() {
    if (cq_ring_ptr_ && cq_ring_ptr_ != MAP_FAILED &&
        cq_ring_ptr_ != sq_ring_ptr_) {
        ::munmap(cq_ring_ptr_, cq_ring_size_);
    }
    cq_ring_ptr_ = nullptr;
    if (sqes_ && sqes_ != MAP_FAILED) {
        ::munmap(sqes_, sqes_size_);
        sqes_ = nullptr;
    }
    if (sq_ring_ptr_ && sq_ring_ptr_ != MAP_FAILED) {
        ::munmap(sq_ring_ptr_, sq_ring_size_);
        sq_ring_ptr_ = nullptr;
    }
    if (ring_fd_ >= 0) {
        if (is_fd_registered_) {
            RsrcUpdate update{};
            update.offset = static_cast<unsigned>(ring_fd_);
            (void)register_call(UNREGISTER_RING_FDS, &update, 1);
        } else {
            // A registered slot holds the ring open until the thread exits.
            if ((enter_flags_ & IORING_ENTER_REGISTERED_RING) != 0) {
                RsrcUpdate update{};
                update.offset = static_cast<unsigned>(enter_fd_);
                (void)register_call(UNREGISTER_RING_FDS, &update, 1);
            }
            ::close(ring_fd_);
        }
        ring_fd_ = -1;
    }
}

struct io_uring_sqe* Ring::get_sqe() {
    const unsigned head =
        std::atomic_ref<unsigned>(*sq_head_).load(std::memory_order_acquire);
    if (sqe_tail_ - head >= *sq_ring_entries_) return nullptr;
    const unsigned index = sqe_tail_ & *sq_ring_mask_;
    ++sqe_tail_;
    ++pending_;
    return &sqes_[index << sqe_shift_];
}

struct io_uring_sqe* Ring::get_sqe128() {
    if (!is_sqe_mixed_) return sqe_shift_ != 0 ? get_sqe() : nullptr;
    const unsigned head =
        std::atomic_ref<unsigned>(*sq_head_).load(std::memory_order_acquire);
    const unsigned entries = *sq_ring_entries_;
    const unsigned index = sqe_tail_ & *sq_ring_mask_;
    // Both slots must be neighbours, so the last slot gets a padding NOP.
    const unsigned pad = index + 1 == entries ? 1U : 0U;
    if (sqe_tail_ - head + pad + 2 > entries) return nullptr;
    if (pad != 0) {
        struct io_uring_sqe* const nop = get_sqe();
        prep_nop(nop);
        nop->flags |= IOSQE_CQE_SKIP_SUCCESS;
    }
    const unsigned first = sqe_tail_ & *sq_ring_mask_;
    sqe_tail_ += 2;
    pending_ += 2;
    return &sqes_[first];
}

void Ring::publish_sqes() {
    std::atomic_ref<unsigned>(*sq_tail_).store(sqe_tail_,
                                               std::memory_order_release);
}

// After __io_uring_flush_sq and sq_ring_needs_enter in liburing, src/queue.c,
// https://github.com/axboe/liburing (MIT), Copyright 2020 Jens Axboe.
// Sequentially consistent: the kernel thread sets NEED_WAKEUP and then reads
// the tail, so either it sees these SQEs or this reads the flag.
int Ring::enter_polled(bool wait) {
    std::atomic_ref<unsigned>(*sq_tail_).store(sqe_tail_,
                                               std::memory_order_seq_cst);
    unsigned flags = wait ? IORING_ENTER_GETEVENTS : 0U;
    if ((std::atomic_ref<unsigned>(*sq_flags_).load(std::memory_order_seq_cst) &
         IORING_SQ_NEED_WAKEUP) != 0) {
        flags |= IORING_ENTER_SQ_WAKEUP;
    }
    const unsigned head =
        std::atomic_ref<unsigned>(*sq_head_).load(std::memory_order_acquire);
    if (sqe_tail_ - head >= *sq_ring_entries_) flags |= IORING_ENTER_SQ_WAIT;
    const unsigned taken = pending_;
    pending_ = 0;
    if (flags == 0) return static_cast<int>(taken);
    if (sys_io_uring_enter(enter_fd_, taken, wait ? 1 : 0,
                           flags | enter_flags_) < 0) {
        return -errno;
    }
    if ((flags & IORING_ENTER_SQ_WAIT) == 0) return static_cast<int>(taken);
    // A kernel may end the wait before it publishes the head it moved.
    while (sqe_tail_ - std::atomic_ref<unsigned>(*sq_head_).load(
                           std::memory_order_acquire) >=
           *sq_ring_entries_) {
        ::sched_yield();
        if (sys_io_uring_enter(enter_fd_, 0, 0,
                               IORING_ENTER_SQ_WAIT | enter_flags_) < 0) {
            return -errno;
        }
    }
    return static_cast<int>(taken);
}

int Ring::enter(bool wait) {
    if (is_sq_polled_) return enter_polled(wait);
    if (pending_ == 0 && !wait) return 0;
    if (!is_sq_rewound_) publish_sqes();
    const int taken = sys_io_uring_enter(
        enter_fd_, pending_, wait ? 1 : 0,
        (wait || deferred_ || is_iopoll_ ? IORING_ENTER_GETEVENTS : 0U) |
            enter_flags_);
    if (taken < 0) return -errno;
    pending_ -= static_cast<unsigned>(taken);
    // This mode reads from slot 0 at each enter and keeps the rings' head and
    // tail at 0.
    if (is_sq_rewound_) sqe_tail_ = pending_;
    return taken;
}

int Ring::enter_wait(std::int64_t timeout_ms) {
    if (!is_sq_rewound_) publish_sqes();
    KernelTimespec timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000000};
    GeteventsArg arg{};
    arg.sigmask_sz = _NSIG / 8;
    arg.ts = reinterpret_cast<std::uint64_t>(&timeout);
    const int taken = static_cast<int>(
        syscall(__NR_io_uring_enter, enter_fd_, pending_, 1,
                IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG | enter_flags_,
                &arg, sizeof(arg)));
    if (taken < 0) return -errno;
    pending_ -= static_cast<unsigned>(taken);
    if (is_sq_rewound_) sqe_tail_ = pending_;
    return taken;
}

bool Ring::enable() {
    return register_call(IORING_REGISTER_ENABLE_RINGS, nullptr, 0) == 0;
}

void Ring::flush_completions() {
    const unsigned wanted =
        IORING_SQ_CQ_OVERFLOW | (deferred_ ? IORING_SQ_TASKRUN : 0U);
    if (!is_iopoll_ &&
        (std::atomic_ref<unsigned>(*sq_flags_).load(std::memory_order_acquire) &
         wanted) == 0) {
        return;
    }
    (void)sys_io_uring_enter(enter_fd_, 0, 0,
                             IORING_ENTER_GETEVENTS | enter_flags_);
}

bool Ring::probe(Probe& out) {
    std::memset(&out, 0, sizeof(out));
    return register_call(REGISTER_PROBE, &out, PROBE_OPS) == 0;
}

void Ring::limit_kernel_workers(unsigned bounded) {
    unsigned limits[2] = {bounded, 0};
    // Older kernels refuse it and keep their default.
    (void)register_call(REGISTER_IOWQ_MAX_WORKERS, limits, 2);
}

bool Ring::has_cqe() const {
    return std::atomic_ref<unsigned>(*cq_head_).load(
               std::memory_order_acquire) !=
           std::atomic_ref<unsigned>(*cq_tail_).load(std::memory_order_acquire);
}

struct io_uring_cqe* Ring::peek_cqe() {
    if (cq_seen_ == cq_visible_tail_) {
        if (cq_committed_ != cq_seen_) {
            std::atomic_ref<unsigned>(*cq_head_).store(
                cq_seen_, std::memory_order_release);
            cq_committed_ = cq_seen_;
        }
        cq_visible_tail_ = std::atomic_ref<unsigned>(*cq_tail_).load(
            std::memory_order_acquire);
        if (cq_seen_ == cq_visible_tail_) return nullptr;
    }
    struct io_uring_cqe* const cqe =
        &cqes_[(cq_seen_ & *cq_ring_mask_) << cqe_shift_];
    // A mixed CQ pads its last slot when a big entry would wrap.
    if (is_cqe_mixed_ && (cqe->flags & IORING_CQE_F_SKIP) != 0) {
        ++cq_seen_;
        return peek_cqe();
    }
    return cqe;
}

void Ring::cqe_seen() {
    if (is_cqe_mixed_ &&
        (cqes_[cq_seen_ & *cq_ring_mask_].flags & IORING_CQE_F_32) != 0) {
        ++cq_seen_;
    }
    ++cq_seen_;
}

}  // namespace uring

// ============================================================================
// IoUringBackend implementation
// ============================================================================

namespace {

constexpr unsigned RING_ENTRIES = 256;

// One per worker that does I/O, so its size counts against the memory budget.
// A burst past it waits in the worker's backlog.
constexpr unsigned WORKER_RING_ENTRIES = 64;

// Each thread that submits gets its own kernel workers, so the default of four
// per CPU would multiply by the workers.
constexpr unsigned WORKER_RING_KERNEL_WORKERS = 4;

// The CQ room a worker's ops leave for its wake and timeout requests, so that a
// worker in a long task never needs an enter to show a completion to a thief.
constexpr unsigned WORKER_CQ_RESERVE = 2;

// Values of user_data that name no operation, which an IoOp address never is.
void* const WAKE_TAG = reinterpret_cast<void*>(std::uintptr_t{1});
void* const CANCEL_TAG = reinterpret_cast<void*>(std::uintptr_t{2});
void* const TIMEOUT_TAG = reinterpret_cast<void*>(std::uintptr_t{3});

// The most bytes one read or write moves, as read(2) and write(2) define it.
constexpr std::size_t MAX_RW_BYTES = 0x7ffff000;

unsigned rw_length(const IoOp& op) noexcept {
    return static_cast<unsigned>(std::min(op.length, MAX_RW_BYTES));
}

// The kinds that need a probe, because an old kernel lacks their opcode.
constexpr IoOpKind PROBED_KINDS[] = {
    IoOpKind::Ftruncate, IoOpKind::Statx,         IoOpKind::Unlink,
    IoOpKind::Rename,    IoOpKind::Mkdir,         IoOpKind::Fallocate,
    IoOpKind::Fadvise,   IoOpKind::SyncFileRange, IoOpKind::Splice,
    IoOpKind::Openat2,   IoOpKind::Linkat,        IoOpKind::Symlinkat,
    IoOpKind::Getxattr,  IoOpKind::Setxattr,      IoOpKind::Fgetxattr,
    IoOpKind::Fsetxattr, IoOpKind::Socket,        IoOpKind::Connect,
    IoOpKind::Bind,      IoOpKind::Listen,        IoOpKind::Shutdown,
    IoOpKind::Sendmsg,   IoOpKind::Recvmsg,       IoOpKind::Tee,
    IoOpKind::Pipe,      IoOpKind::Madvise,       IoOpKind::Waitid,
    IoOpKind::FutexWait, IoOpKind::FutexWake,     IoOpKind::FutexWaitv,
    IoOpKind::EpollCtl,  IoOpKind::EpollWait,     IoOpKind::PollAdd,
};

unsigned probed_opcode(IoOpKind kind) noexcept {
    switch (kind) {
        case IoOpKind::Ftruncate:
            return uring::OP_FTRUNCATE;
        case IoOpKind::Statx:
            return uring::OP_STATX;
        case IoOpKind::Unlink:
            return uring::OP_UNLINKAT;
        case IoOpKind::Rename:
            return uring::OP_RENAMEAT;
        case IoOpKind::Mkdir:
            return uring::OP_MKDIRAT;
        case IoOpKind::Fallocate:
            return uring::OP_FALLOCATE;
        case IoOpKind::Fadvise:
            return uring::OP_FADVISE;
        case IoOpKind::SyncFileRange:
            return uring::OP_SYNC_FILE_RANGE;
        case IoOpKind::Splice:
            return uring::OP_SPLICE;
        case IoOpKind::Openat2:
            return uring::OP_OPENAT2;
        case IoOpKind::Linkat:
            return uring::OP_LINKAT;
        case IoOpKind::Symlinkat:
            return uring::OP_SYMLINKAT;
        case IoOpKind::Getxattr:
            return uring::OP_GETXATTR;
        case IoOpKind::Setxattr:
            return uring::OP_SETXATTR;
        case IoOpKind::Fgetxattr:
            return uring::OP_FGETXATTR;
        case IoOpKind::Fsetxattr:
            return uring::OP_FSETXATTR;
        case IoOpKind::Socket:
            return uring::OP_SOCKET;
        case IoOpKind::Connect:
            return uring::OP_CONNECT;
        case IoOpKind::Bind:
            return uring::OP_BIND;
        case IoOpKind::Listen:
            return uring::OP_LISTEN;
        case IoOpKind::Shutdown:
            return uring::OP_SHUTDOWN;
        case IoOpKind::Sendmsg:
            return uring::OP_SENDMSG;
        case IoOpKind::Recvmsg:
            return uring::OP_RECVMSG;
        case IoOpKind::Tee:
            return uring::OP_TEE;
        case IoOpKind::Pipe:
            return uring::OP_PIPE;
        case IoOpKind::Madvise:
            return uring::OP_MADVISE;
        case IoOpKind::Waitid:
            return uring::OP_WAITID;
        case IoOpKind::FutexWait:
            return uring::OP_FUTEX_WAIT;
        case IoOpKind::FutexWake:
            return uring::OP_FUTEX_WAKE;
        case IoOpKind::FutexWaitv:
            return uring::OP_FUTEX_WAITV;
        case IoOpKind::EpollCtl:
            return uring::OP_EPOLL_CTL;
        case IoOpKind::EpollWait:
            return uring::OP_EPOLL_WAIT;
        case IoOpKind::PollAdd:
            return uring::OP_POLL_ADD;
        default:
            return uring::PROBE_OPS;
    }
}

constexpr std::uint64_t kind_bit(IoOpKind kind) noexcept {
    return std::uint64_t{1} << static_cast<unsigned>(kind);
}

// The SQE holds a 32 bit length.
bool can_fit_request(const IoOp& op) noexcept {
    switch (op.kind) {
        case IoOpKind::Fadvise:
        case IoOpKind::SyncFileRange:
        case IoOpKind::Madvise:
            return op.length <= std::numeric_limits<std::uint32_t>::max();
        case IoOpKind::Getxattr:
        case IoOpKind::Setxattr:
        case IoOpKind::Fgetxattr:
        case IoOpKind::Fsetxattr:
            return op.xattr->size <= std::numeric_limits<std::uint32_t>::max();
        default:
            return true;
    }
}

bool can_wait_for_peer(IoOpKind kind) noexcept {
    switch (kind) {
        case IoOpKind::Read:
        case IoOpKind::Readv:
        case IoOpKind::Recv:
        case IoOpKind::Accept:
        case IoOpKind::Send:
        case IoOpKind::Splice:
        case IoOpKind::Connect:
        case IoOpKind::Sendmsg:
        case IoOpKind::Recvmsg:
        case IoOpKind::PollAdd:
        case IoOpKind::Waitid:
        case IoOpKind::FutexWait:
        case IoOpKind::FutexWaitv:
        case IoOpKind::EpollWait:
        case IoOpKind::Tee:
            return true;
        default:
            return false;
    }
}

// These end by themselves, so a worker can always drain them before it exits. A
// sequential read or write may be on a pipe, and an open may be on a FIFO.
bool runs_on_worker_ring(IoOpKind kind) noexcept {
    switch (kind) {
        case IoOpKind::Pread:
        case IoOpKind::Pwrite:
        case IoOpKind::Preadv:
        case IoOpKind::Pwritev:
        case IoOpKind::Close:
        case IoOpKind::Fsync:
        case IoOpKind::Ftruncate:
        case IoOpKind::Statx:
        case IoOpKind::Unlink:
        case IoOpKind::Rename:
        case IoOpKind::Mkdir:
        case IoOpKind::Fallocate:
        case IoOpKind::Fadvise:
        case IoOpKind::SyncFileRange:
        case IoOpKind::Linkat:
        case IoOpKind::Symlinkat:
        case IoOpKind::Getxattr:
        case IoOpKind::Setxattr:
        case IoOpKind::Fgetxattr:
        case IoOpKind::Fsetxattr:
        case IoOpKind::Socket:
        case IoOpKind::Bind:
        case IoOpKind::Listen:
        case IoOpKind::Shutdown:
        case IoOpKind::Pipe:
        case IoOpKind::Madvise:
        case IoOpKind::EpollCtl:
        case IoOpKind::FutexWake:
            return true;
        default:
            return false;
    }
}

void prepare(struct io_uring_sqe* sqe, IoOp& op) noexcept {
    // Offset -1 uses the current file position.
    switch (op.kind) {
        case IoOpKind::Read:
            uring::prep_read(sqe, op.handle, op.buffer, rw_length(op), -1);
            break;
        case IoOpKind::Write:
            uring::prep_write(sqe, op.handle, op.buffer, rw_length(op), -1);
            break;
        case IoOpKind::Pread:
            uring::prep_read(sqe, op.handle, op.buffer, rw_length(op),
                             op.offset);
            break;
        case IoOpKind::Pwrite:
            uring::prep_write(sqe, op.handle, op.buffer, rw_length(op),
                              op.offset);
            break;
        case IoOpKind::Open:
            uring::prep_openat(sqe, AT_FDCWD, op.path, op.flags, op.mode);
            break;
        case IoOpKind::Close:
            uring::prep_close(sqe, op.handle);
            break;
        case IoOpKind::Fsync:
            uring::prep_fsync(sqe, op.handle, 0);
            break;
        case IoOpKind::Accept:
            uring::prep_accept(sqe, op.handle, op.addr, op.addrlen,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
            break;
        case IoOpKind::Recv:
            uring::prep_recv(sqe, op.handle, op.buffer, rw_length(op),
                             op.flags);
            break;
        case IoOpKind::Send:
            uring::prep_send(sqe, op.handle, op.buffer, rw_length(op),
                             op.flags);
            break;
        case IoOpKind::Readv:
            uring::prep_readv(sqe, op.handle, op.iov,
                              static_cast<unsigned>(op.iov_count), -1);
            break;
        case IoOpKind::Writev:
            uring::prep_writev(sqe, op.handle, op.iov,
                               static_cast<unsigned>(op.iov_count), -1);
            break;
        case IoOpKind::Preadv:
            uring::prep_readv(sqe, op.handle, op.iov,
                              static_cast<unsigned>(op.iov_count), op.offset);
            break;
        case IoOpKind::Pwritev:
            uring::prep_writev(sqe, op.handle, op.iov,
                               static_cast<unsigned>(op.iov_count), op.offset);
            break;
        case IoOpKind::Ftruncate:
            uring::prep_ftruncate(sqe, op.handle, op.offset);
            break;
        case IoOpKind::Statx:
            uring::prep_statx(sqe, op.handle, op.path, op.flags, op.statx_mask,
                              op.statx_out);
            break;
        case IoOpKind::Unlink:
            uring::prep_unlinkat(sqe, AT_FDCWD, op.path, op.flags);
            break;
        case IoOpKind::Rename:
            uring::prep_renameat(sqe, AT_FDCWD, op.path, AT_FDCWD, op.path2, 0);
            break;
        case IoOpKind::Mkdir:
            uring::prep_mkdirat(sqe, AT_FDCWD, op.path, op.mode);
            break;
        case IoOpKind::Fallocate:
            uring::prep_fallocate(sqe, op.handle, op.flags,
                                  static_cast<std::uint64_t>(op.offset),
                                  static_cast<std::uint64_t>(op.length));
            break;
        case IoOpKind::Fadvise:
            uring::prep_fadvise(
                sqe, op.handle, static_cast<std::uint64_t>(op.offset),
                static_cast<std::uint32_t>(op.length), op.flags);
            break;
        case IoOpKind::SyncFileRange:
            uring::prep_sync_file_range(
                sqe, op.handle, static_cast<unsigned>(op.length),
                static_cast<std::uint64_t>(op.offset), op.flags);
            break;
        case IoOpKind::Splice:
            uring::prep_splice(sqe, op.handle, op.offset, op.out_handle,
                               op.out_offset, rw_length(op),
                               static_cast<unsigned>(op.flags));
            break;
        case IoOpKind::Openat2:
            uring::prep_openat2(sqe, op.handle, op.path, op.how,
                                sizeof(OpenHow));
            break;
        case IoOpKind::Linkat:
            uring::prep_linkat(sqe, op.handle, op.path, op.out_handle, op.path2,
                               op.flags);
            break;
        case IoOpKind::Symlinkat:
            uring::prep_symlinkat(sqe, op.path, op.handle, op.path2);
            break;
        case IoOpKind::Getxattr:
            uring::prep_getxattr(sqe, op.xattr->name,
                                 static_cast<char*>(op.xattr->value), op.path,
                                 static_cast<unsigned>(op.xattr->size));
            break;
        case IoOpKind::Setxattr:
            uring::prep_setxattr(sqe, op.xattr->name,
                                 static_cast<const char*>(op.xattr->value),
                                 op.path, op.xattr->flags,
                                 static_cast<unsigned>(op.xattr->size));
            break;
        case IoOpKind::Fgetxattr:
            uring::prep_fgetxattr(sqe, op.handle, op.xattr->name,
                                  static_cast<char*>(op.xattr->value),
                                  static_cast<unsigned>(op.xattr->size));
            break;
        case IoOpKind::Fsetxattr:
            uring::prep_fsetxattr(sqe, op.handle, op.xattr->name,
                                  static_cast<const char*>(op.xattr->value),
                                  op.xattr->flags,
                                  static_cast<unsigned>(op.xattr->size));
            break;
        case IoOpKind::Socket:
            uring::prep_socket(sqe, op.handle, static_cast<int>(op.offset),
                               op.flags, 0);
            break;
        case IoOpKind::Connect:
            uring::prep_connect(sqe, op.handle, op.addr,
                                static_cast<socklen_t>(op.length));
            break;
        case IoOpKind::Bind:
            uring::prep_bind(sqe, op.handle, op.addr,
                             static_cast<socklen_t>(op.length));
            break;
        case IoOpKind::Listen:
            uring::prep_listen(sqe, op.handle, op.flags);
            break;
        case IoOpKind::Shutdown:
            uring::prep_shutdown(sqe, op.handle, op.flags);
            break;
        case IoOpKind::Sendmsg:
            uring::prep_sendmsg(sqe, op.handle, op.msg,
                                static_cast<unsigned>(op.flags));
            break;
        case IoOpKind::Recvmsg:
            uring::prep_recvmsg(sqe, op.handle, op.msg,
                                static_cast<unsigned>(op.flags));
            break;
        case IoOpKind::Tee:
            uring::prep_tee(sqe, op.handle, op.out_handle, rw_length(op),
                            static_cast<unsigned>(op.flags));
            break;
        case IoOpKind::Pipe:
            uring::prep_pipe(sqe, op.pipe_fds, op.flags);
            break;
        case IoOpKind::Madvise:
            uring::prep_madvise(sqe, op.buffer,
                                static_cast<unsigned>(op.length), op.flags);
            break;
        case IoOpKind::Waitid:
            uring::prep_waitid(sqe, op.idtype, op.handle, op.info, op.flags, 0);
            break;
        case IoOpKind::FutexWait:
            uring::prep_futex_wait(
                sqe, op.futex, static_cast<std::uint32_t>(op.offset),
                op.futex_mask, static_cast<std::uint32_t>(op.flags), 0);
            break;
        case IoOpKind::FutexWake:
            uring::prep_futex_wake(
                sqe, op.futex, static_cast<std::uint32_t>(op.offset),
                op.futex_mask, static_cast<std::uint32_t>(op.flags), 0);
            break;
        case IoOpKind::FutexWaitv:
            uring::prep_futex_waitv(sqe, op.futex_waitv,
                                    static_cast<unsigned>(op.length), 0);
            break;
        case IoOpKind::EpollCtl:
            uring::prep_epoll_ctl(sqe, op.handle, static_cast<int>(op.offset),
                                  op.flags, op.epoll_events);
            break;
        case IoOpKind::EpollWait:
            uring::prep_epoll_wait(sqe, op.handle, op.epoll_events, op.flags,
                                   0);
            break;
        case IoOpKind::PollAdd:
            uring::prep_poll_add(sqe, op.handle,
                                 static_cast<unsigned>(op.flags));
            break;
        case IoOpKind::Fstat:
        case IoOpKind::Lseek:
        case IoOpKind::Sendfile:
        case IoOpKind::Call:
            break;
    }
    uring::sqe_set_data(sqe, &op);
}

}  // namespace

struct IoUringBackend::WorkerRing {
    static constexpr std::int64_t NO_TIMEOUT =
        std::numeric_limits<std::int64_t>::max();

    // The one reader of the CQ. A parked ring keeps its reader, because the
    // wake request must not vanish between the check and the sleep.
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) std::atomic<bool> reaping{false};
    std::atomic<bool> live{false};
    std::atomic<bool> parked{false};
    std::atomic<bool> claimed{false};
    std::atomic<std::size_t> in_flight{0};
    std::atomic<unsigned> in_ring{0};
    // Read under `reaping` or by the ring's thread.
    bool wake_armed = false;
    // When the soonest armed timeout ends, as far as one is known to be armed.
    std::int64_t soonest_timeout_ms = NO_TIMEOUT;

    // Only the ring's thread touches these.
    uring::Ring ring;
    uring::KernelTimespec park_timeout{};
    IoOp* backlog_head = nullptr;
    IoOp* backlog_tail = nullptr;
    bool ring_failed = false;

    std::uint64_t wake_value = 0;
    dstd::ScopedFd wake_fd;
    WorkerRing* next = nullptr;
    dstd::UniquePtr<WorkerRing> owned_next;

    // Sequentially consistent, as the park handshake reads `parked` around it.
    void lock() noexcept {
        while (reaping.exchange(true, std::memory_order_seq_cst)) {
            DFTRACER_UTILS_CPU_PAUSE();
        }
    }
    bool try_lock() noexcept {
        return !reaping.load(std::memory_order_relaxed) &&
               !reaping.exchange(true, std::memory_order_seq_cst);
    }
    void unlock() noexcept { reaping.store(false, std::memory_order_release); }
};

namespace {

struct AttachedRing {
    const void* backend = nullptr;
    void* ring = nullptr;
};

thread_local AttachedRing t_attached;

}  // namespace

IoUringBackend::IoUringBackend(std::size_t pool_size,
                               IoPoolOptions pool_options,
                               unsigned masked_features,
                               std::uint64_t masked_kinds,
                               unsigned sq_poll_idle_ms)
    : ThreadPoolFileOps(pool_size, 0, pool_options),
      masked_features_(masked_features),
      masked_kinds_(masked_kinds),
      sq_poll_idle_ms_(sq_poll_idle_ms) {}

IoUringBackend::~IoUringBackend() = default;

bool IoUringBackend::probe() {
    uring::Ring test_ring;
    // The size start() asks for, so that a pass here holds there on a kernel
    // that counts a ring against the locked memory limit.
    if (!test_ring.init(RING_ENTRIES)) return false;
    read_opcodes(test_ring);
    // The ring runs sequential reads and writes at the file position.
    return (test_ring.features() & ~masked_features_ &
            IORING_FEAT_RW_CUR_POS) != 0;
}

void IoUringBackend::read_opcodes(uring::Ring& ring) noexcept {
    probed_kinds_ = 0;
    uring::Probe probe;
    if (!ring.probe(probe)) return;
    for (const IoOpKind kind : PROBED_KINDS) {
        if (probe.has(probed_opcode(kind))) probed_kinds_ |= kind_bit(kind);
    }
}

bool IoUringBackend::has_opcode(IoOpKind kind) const noexcept {
    if ((masked_kinds_ & kind_bit(kind)) != 0) return false;
    switch (kind) {
        case IoOpKind::Read:
        case IoOpKind::Write:
        case IoOpKind::Pread:
        case IoOpKind::Pwrite:
        case IoOpKind::Open:
        case IoOpKind::Close:
        case IoOpKind::Fsync:
        case IoOpKind::Accept:
        case IoOpKind::Recv:
        case IoOpKind::Send:
        case IoOpKind::Readv:
        case IoOpKind::Writev:
        case IoOpKind::Preadv:
        case IoOpKind::Pwritev:
            return true;
        case IoOpKind::Ftruncate:
        case IoOpKind::Statx:
        case IoOpKind::Unlink:
        case IoOpKind::Rename:
        case IoOpKind::Mkdir:
        case IoOpKind::Fallocate:
        case IoOpKind::Fadvise:
        case IoOpKind::SyncFileRange:
        case IoOpKind::Splice:
        case IoOpKind::Openat2:
        case IoOpKind::Linkat:
        case IoOpKind::Symlinkat:
        case IoOpKind::Getxattr:
        case IoOpKind::Setxattr:
        case IoOpKind::Fgetxattr:
        case IoOpKind::Fsetxattr:
        case IoOpKind::Socket:
        case IoOpKind::Connect:
        case IoOpKind::Bind:
        case IoOpKind::Listen:
        case IoOpKind::Shutdown:
        case IoOpKind::Sendmsg:
        case IoOpKind::Recvmsg:
        case IoOpKind::Tee:
        case IoOpKind::Pipe:
        case IoOpKind::Madvise:
        case IoOpKind::Waitid:
        case IoOpKind::FutexWait:
        case IoOpKind::FutexWake:
        case IoOpKind::FutexWaitv:
        case IoOpKind::EpollCtl:
        case IoOpKind::EpollWait:
        case IoOpKind::PollAdd:
            return (probed_kinds_ & kind_bit(kind)) != 0;
        case IoOpKind::Fstat:
        case IoOpKind::Lseek:
        case IoOpKind::Sendfile:
        case IoOpKind::Call:
            return false;
    }
    return false;
}

dstd::Result<void> IoUringBackend::start() {
    cancelling_.store(false, std::memory_order_release);
    cancel_requested_.store(false, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    // A polled ring needs no fixed files only since 5.11, and an older kernel
    // also asks for privileges. So this asks and says when it runs without.
    is_sq_polled_ =
        sq_poll_idle_ms_ != 0 &&
        ring_.init(RING_ENTRIES, IORING_SETUP_SQPOLL, sq_poll_idle_ms_) &&
        (ring_.features() & ~masked_features_ & IORING_FEAT_SQPOLL_NONFIXED) !=
            0;
    if (sq_poll_idle_ms_ != 0 && !is_sq_polled_) {
        ring_.destroy();
        DFTRACER_UTILS_LOG_INFO("{}",
                                "io_uring: this kernel refuses SQPOLL, "
                                "running without it");
    }
    // Only the ring thread enters this ring, so completions can wait for it.
    // The thread takes the ring when it enables it. A kernel before 6.1 refuses
    // these flags.
    should_enable_ring_ =
        !is_sq_polled_ &&
        ring_.init(RING_ENTRIES,
                   IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
                       IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_R_DISABLED);
    if (!is_sq_polled_ && !should_enable_ring_ && !ring_.init(RING_ENTRIES)) {
        return common::make_error<"io_uring setup failed: {}">(
            common::ErrorCode::Internal, std::strerror(errno));
    }
    timed_enter_ =
        (ring_.features() & ~masked_features_ & IORING_FEAT_EXT_ARG) != 0;
    {
        // Not ring_, which may start disabled and refuse a register call.
        uring::Ring probe_ring;
        if (probe_ring.init(2)) {
            read_opcodes(probe_ring);
        } else {
            probed_kinds_ = 0;
        }
    }
    // Blocking, because the ring gets -EAGAIN from a non-blocking eventfd
    // instead of waiting for it.
    wake_fd_.reset(::eventfd(0, EFD_CLOEXEC));
    if (!wake_fd_.valid()) {
        const int error = errno;
        ring_.destroy();
        return common::make_error<"eventfd failed: {}">(
            common::ErrorCode::Internal, std::strerror(error));
    }
    if (auto started = pool_.start(); !started) {
        stop();
        return started;
    }
    enable_result_.store(0, std::memory_order_relaxed);
    thread_ = dstd::Thread([this] { ring_loop(); });
    if (!should_enable_ring_) return {};
    int enabled = 0;
    while ((enabled = enable_result_.load(std::memory_order_acquire)) == 0) {
        std::this_thread::yield();
    }
    if (enabled < 0) {
        stop();
        return common::make_error<"io_uring enable failed: {}">(
            common::ErrorCode::Internal, std::strerror(-enabled));
    }
    return {};
}

void IoUringBackend::stop() {
    stopping_.store(true, std::memory_order_seq_cst);
    wake();
    if (thread_.joinable()) thread_.join();
    pool_.stop();
    ring_.destroy();
    wake_fd_.reset();
    // Every worker has detached. Iterative, as the chain can be long.
    worker_rings_.store(nullptr, std::memory_order_release);
    dstd::UniquePtr<WorkerRing> rings = std::move(worker_rings_owned_);
    while (rings) rings = std::move(rings->owned_next);
}

std::size_t IoUringBackend::in_flight() const {
    std::size_t total = 0;
    // The rings first: a completion on a thread with no ring resubmits to the
    // owner before its ring's count falls, so an op moves only that way.
    for (const WorkerRing* worker =
             worker_rings_.load(std::memory_order_acquire);
         worker != nullptr; worker = worker->next) {
        total += worker->in_flight.load(std::memory_order_acquire);
    }
    return total + in_flight_.load(std::memory_order_acquire) +
           ThreadPoolFileOps::in_flight();
}

void IoUringBackend::attach_thread() {
    std::lock_guard<std::mutex> lock(worker_rings_mutex_);
    for (WorkerRing* worker = worker_rings_.load(std::memory_order_relaxed);
         worker != nullptr; worker = worker->next) {
        bool expected = false;
        if (worker->claimed.compare_exchange_strong(expected, true)) {
            t_attached = {this, worker};
            return;
        }
    }
    auto node = dstd::make_unique<WorkerRing>();
    WorkerRing* const worker = node.get();
    worker->claimed.store(true, std::memory_order_relaxed);
    worker->next = worker_rings_.load(std::memory_order_relaxed);
    worker->owned_next = std::move(worker_rings_owned_);
    worker_rings_owned_ = std::move(node);
    worker_rings_.store(worker, std::memory_order_release);
    t_attached = {this, worker};
}

IoUringBackend::WorkerRing* IoUringBackend::calling_worker_ring(
    IoOpKind kind) noexcept {
    // Each polled ring costs a kernel thread that spins, so there is only one.
    if (t_attached.backend != this || is_sq_polled_ ||
        !runs_on_worker_ring(kind)) {
        return nullptr;
    }
    auto* worker = static_cast<WorkerRing*>(t_attached.ring);
    if (worker->live.load(std::memory_order_relaxed)) return worker;
    // A worker that never does I/O costs no ring. One the kernel refuses, such
    // as past the file limit, leaves this worker on the owner's ring.
    if (worker->ring_failed) return nullptr;
    worker->lock();
    // Not DEFER_TASKRUN: since some kernel after 6.1 it hides a completion
    // until the owner enters, so a worker in a long task would hold it from
    // thieves and the sysmon. A kernel before 6.0 refuses this flag.
    const bool created =
        worker->ring.init(WORKER_RING_ENTRIES, IORING_SETUP_SINGLE_ISSUER) ||
        worker->ring.init(WORKER_RING_ENTRIES);
    if (created) {
        worker->ring.limit_kernel_workers(WORKER_RING_KERNEL_WORKERS);
        worker->live.store(true, std::memory_order_release);
    }
    worker->unlock();
    worker->ring_failed = !created;
    return created ? worker : nullptr;
}

void IoUringBackend::fill_worker(WorkerRing& worker) noexcept {
    while (worker.backlog_head != nullptr &&
           worker.in_ring.load(std::memory_order_relaxed) <
               worker.ring.cq_entries() - WORKER_CQ_RESERVE) {
        struct io_uring_sqe* sqe = worker.ring.get_sqe();
        if (sqe == nullptr) {
            if (worker.ring.enter(false) <= 0) return;
            sqe = worker.ring.get_sqe();
            if (sqe == nullptr) return;
        }
        IoOp& op = *worker.backlog_head;
        worker.backlog_head = op.next;
        if (worker.backlog_head == nullptr) worker.backlog_tail = nullptr;
        prepare(sqe, op);
        // A thief completes it, ordered only through the kernel.
        DFTRACER_UTILS_TSAN_RELEASE(&op);
        worker.in_ring.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t IoUringBackend::drain_worker(WorkerRing& worker) noexcept {
    std::size_t reaped = 0;
    while (struct io_uring_cqe* cqe = worker.ring.peek_cqe()) {
        void* const data = reinterpret_cast<void*>(cqe->user_data);
        const ssize_t res = cqe->res;
        worker.ring.cqe_seen();
        worker.in_ring.fetch_sub(1, std::memory_order_relaxed);
        if (data == WAKE_TAG) {
            worker.wake_armed = false;
            continue;
        }
        if (data == TIMEOUT_TAG) {
            worker.soonest_timeout_ms = WorkerRing::NO_TIMEOUT;
            continue;
        }
        ++reaped;
        auto& op = *static_cast<IoOp*>(data);
        DFTRACER_UTILS_TSAN_ACQUIRE(&op);
        op.result = res;
        op.complete(op);
        // After the complete, so a quiesce that reads zero sees its task.
        worker.in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
    return reaped;
}

std::size_t IoUringBackend::reap_worker(WorkerRing& worker) noexcept {
    if (!worker.ring.has_cqe()) return 0;
    // A completion that submits and flushes lands here on its own ring, and the
    // drain further up takes what that added.
    if (!worker.try_lock()) return 0;
    const std::size_t reaped = drain_worker(worker);
    worker.unlock();
    return reaped;
}

void IoUringBackend::flush_worker(WorkerRing& worker) noexcept {
    fill_worker(worker);
    if (worker.ring.pending() != 0) {
        const int entered = worker.ring.enter(false);
        // What it did not take stays pending for the next flush or the park.
        if (entered < 0 && entered != -EAGAIN && entered != -EBUSY &&
            entered != -EINTR) {
            DFTRACER_UTILS_LOG_ERROR("io_uring_enter failed: {}",
                                     std::strerror(-entered));
        }
    }
    worker.ring.flush_completions();
    reap_worker(worker);
}

// Thieves stay out from here on. One already inside finishes first.
void IoUringBackend::quiet_worker(WorkerRing& worker) noexcept {
    worker.parked.store(true, std::memory_order_seq_cst);
    worker.lock();
    worker.unlock();
}

void IoUringBackend::detach_thread() {
    if (t_attached.backend != this) return;
    auto* worker = static_cast<WorkerRing*>(t_attached.ring);
    t_attached = {};
    if (worker->live.load(std::memory_order_relaxed)) {
        quiet_worker(*worker);
        // The kernel cancels what an exiting thread submitted.
        while (worker->in_flight.load(std::memory_order_acquire) != 0) {
            fill_worker(*worker);
            const int entered = worker->ring.enter(true);
            if (entered < 0 && entered != -EINTR) std::this_thread::yield();
            reap_worker(*worker);
        }
        worker->lock();
        worker->live.store(false, std::memory_order_release);
        worker->ring.destroy();
        worker->wake_armed = false;
        worker->soonest_timeout_ms = WorkerRing::NO_TIMEOUT;
        worker->in_ring.store(0, std::memory_order_relaxed);
        worker->unlock();
        worker->parked.store(false, std::memory_order_seq_cst);
    }
    worker->ring_failed = false;
    worker->claimed.store(false, std::memory_order_release);
}

void* IoUringBackend::prepare_park() {
    if (t_attached.backend != this) return nullptr;
    auto* worker = static_cast<WorkerRing*>(t_attached.ring);
    if (worker->in_flight.load(std::memory_order_acquire) == 0) return nullptr;
    quiet_worker(*worker);
    if (!worker->wake_fd.valid()) {
        // Blocking, as for the owner's ring.
        worker->wake_fd.reset(::eventfd(0, EFD_CLOEXEC));
    }
    if (!worker->wake_fd.valid()) {
        worker->parked.store(false, std::memory_order_seq_cst);
        return nullptr;
    }
    if (!worker->wake_armed) {
        struct io_uring_sqe* sqe = worker->ring.get_sqe();
        if (sqe == nullptr) {
            worker->ring.enter(false);
            sqe = worker->ring.get_sqe();
        }
        if (sqe == nullptr) {
            worker->parked.store(false, std::memory_order_seq_cst);
            return nullptr;
        }
        uring::prep_read(sqe, worker->wake_fd.get(), &worker->wake_value,
                         sizeof(worker->wake_value), 0);
        uring::sqe_set_data(sqe, WAKE_TAG);
        worker->wake_armed = true;
        worker->in_ring.fetch_add(1, std::memory_order_relaxed);
    }
    return worker;
}

void IoUringBackend::park(void* token, std::int64_t timeout_ms) {
    auto& worker = *static_cast<WorkerRing*>(token);
    fill_worker(worker);
    // -ETIME, -EINTR and a full CQ all come back to the caller's checks.
    if (timed_enter_) {
        (void)worker.ring.enter_wait(timeout_ms);
        return;
    }
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    // One armed by an earlier park that ends sooner only ends this wait early,
    // which those checks allow. One that ends later must not hold this wait, so
    // it keeps running and its completion is dropped.
    if (now_ms + timeout_ms < worker.soonest_timeout_ms) {
        struct io_uring_sqe* sqe = worker.ring.get_sqe();
        if (sqe == nullptr) {
            worker.ring.enter(false);
            sqe = worker.ring.get_sqe();
        }
        if (sqe == nullptr) return;
        worker.park_timeout = {timeout_ms / 1000,
                               (timeout_ms % 1000) * 1000000};
        uring::prep_timeout(sqe, &worker.park_timeout);
        uring::sqe_set_data(sqe, TIMEOUT_TAG);
        worker.soonest_timeout_ms = now_ms + timeout_ms;
        worker.in_ring.fetch_add(1, std::memory_order_relaxed);
    }
    (void)worker.ring.enter(true);
}

void IoUringBackend::unpark(void* token) noexcept {
    auto& worker = *static_cast<WorkerRing*>(token);
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t written =
        ::write(worker.wake_fd.get(), &one, sizeof(one));
}

void IoUringBackend::finish_park(void* token) {
    auto& worker = *static_cast<WorkerRing*>(token);
    worker.parked.store(false, std::memory_order_seq_cst);
    worker.ring.flush_completions();
    reap_worker(worker);
}

void IoUringBackend::submit(IoOp& op) {
    if (!has_opcode(op.kind) || !can_fit_request(op)) {
        ThreadPoolFileOps::submit(op);
        return;
    }
    if (WorkerRing* worker = calling_worker_ring(op.kind)) {
        worker->in_flight.fetch_add(1, std::memory_order_acq_rel);
        op.next = nullptr;
        if (worker->backlog_tail == nullptr) {
            worker->backlog_head = &op;
        } else {
            worker->backlog_tail->next = &op;
        }
        worker->backlog_tail = &op;
        return;
    }
    in_flight_.fetch_add(1, std::memory_order_acq_rel);
    IoOp* head = incoming_.load(std::memory_order_relaxed);
    do {
        op.next = head;
    } while (!incoming_.compare_exchange_weak(
        head, &op, std::memory_order_seq_cst, std::memory_order_relaxed));
}

int IoUringBackend::flush() {
    const int woken = ThreadPoolFileOps::flush();
    if (t_attached.backend == this) {
        auto* worker = static_cast<WorkerRing*>(t_attached.ring);
        if (worker->live.load(std::memory_order_relaxed)) flush_worker(*worker);
    }
    if (incoming_.load(std::memory_order_seq_cst) != nullptr &&
        parked_.load(std::memory_order_seq_cst) &&
        parked_.exchange(false, std::memory_order_seq_cst)) {
        wake();
    }
    return woken;
}

std::size_t IoUringBackend::poll(int timeout_ms) {
    (void)timeout_ms;
    std::size_t reaped = 0;
    const void* const own =
        t_attached.backend == this ? t_attached.ring : nullptr;
    for (WorkerRing* worker = worker_rings_.load(std::memory_order_acquire);
         worker != nullptr; worker = worker->next) {
        if (worker == own) {
            if (worker->live.load(std::memory_order_relaxed)) {
                reaped += reap_worker(*worker);
            }
            continue;
        }
        if (worker->in_flight.load(std::memory_order_relaxed) == 0 ||
            worker->parked.load(std::memory_order_relaxed) ||
            !worker->try_lock()) {
            continue;
        }
        // Held across the drain: a retiring worker destroys its ring under it.
        if (worker->live.load(std::memory_order_acquire) &&
            !worker->parked.load(std::memory_order_seq_cst)) {
            reaped += drain_worker(*worker);
        }
        worker->unlock();
    }
    return reaped;
}

void IoUringBackend::cancel_pending() {
    ThreadPoolFileOps::cancel_pending();
    cancelling_.store(true, std::memory_order_seq_cst);
    cancel_requested_.store(true, std::memory_order_seq_cst);
    wake();
}

void IoUringBackend::wake() noexcept {
    if (!wake_fd_.valid()) return;
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t written =
        ::write(wake_fd_.get(), &one, sizeof(one));
}

void IoUringBackend::take_incoming() noexcept {
    IoOp* taken = incoming_.exchange(nullptr, std::memory_order_seq_cst);
    IoOp* first = nullptr;
    IoOp* const old_tail = backlog_tail_;
    // The stack holds the newest first.
    while (taken != nullptr) {
        IoOp* const next = taken->next;
        taken->next = first;
        if (first == nullptr) backlog_tail_ = taken;
        first = taken;
        taken = next;
    }
    if (first == nullptr) return;
    if (old_tail == nullptr) {
        backlog_head_ = first;
    } else {
        old_tail->next = first;
    }
}

void IoUringBackend::fail(IoOp& op, ssize_t result) noexcept {
    op.result = result;
    op.complete(op);
    in_flight_.fetch_sub(1, std::memory_order_acq_rel);
}

void IoUringBackend::fill_ring() noexcept {
    if (!wake_armed_) {
        struct io_uring_sqe* sqe = ring_.get_sqe();
        if (sqe != nullptr) {
            uring::prep_read(sqe, wake_fd_.get(), &wake_value_,
                             sizeof(wake_value_), 0);
            uring::sqe_set_data(sqe, WAKE_TAG);
            wake_armed_ = true;
            ++in_ring_;
        }
    }
    const bool cancelling = cancelling_.load(std::memory_order_seq_cst);
    // Bounded by the CQ. An op that waits on a peer does not count: it can sit
    // for ever and must not hold back a file op. The kernel keeps any overflow.
    while (backlog_head_ != nullptr && in_ring_ < ring_.cq_entries()) {
        IoOp& op = *backlog_head_;
        const bool peer = can_wait_for_peer(op.kind);
        if (cancelling && peer) {
            backlog_head_ = op.next;
            if (backlog_head_ == nullptr) backlog_tail_ = nullptr;
            fail(op, -ECANCELED);
            continue;
        }
        struct io_uring_sqe* sqe = ring_.get_sqe();
        if (sqe == nullptr) return;
        backlog_head_ = op.next;
        if (backlog_head_ == nullptr) backlog_tail_ = nullptr;
        prepare(sqe, op);
        if (!peer) {
            ++in_ring_;
        } else {
            op.prev = nullptr;
            op.next = waiting_head_;
            if (waiting_head_ != nullptr) waiting_head_->prev = &op;
            waiting_head_ = &op;
        }
    }
}

std::size_t IoUringBackend::reap() noexcept {
    std::size_t reaped = 0;
    while (struct io_uring_cqe* cqe = ring_.peek_cqe()) {
        void* const data = reinterpret_cast<void*>(cqe->user_data);
        const ssize_t res = cqe->res;
        ring_.cqe_seen();
        ++reaped;
        if (data == WAKE_TAG) {
            --in_ring_;
            wake_armed_ = false;
            continue;
        }
        if (data == CANCEL_TAG) {
            --in_ring_;
            continue;
        }
        auto& op = *static_cast<IoOp*>(data);
        if (can_wait_for_peer(op.kind)) {
            // A cancelled op has left the list and points at itself.
            if (op.prev != &op) unlink_waiting(op);
        } else {
            --in_ring_;
        }
        op.result = res;
        op.complete(op);
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    }
    return reaped;
}

void IoUringBackend::unlink_waiting(IoOp& op) noexcept {
    if (op.prev != nullptr) {
        op.prev->next = op.next;
    } else {
        waiting_head_ = op.next;
    }
    if (op.next != nullptr) op.next->prev = op.prev;
    op.prev = &op;
}

void IoUringBackend::cancel_waiting() noexcept {
    while (waiting_head_ != nullptr) {
        struct io_uring_sqe* sqe = ring_.get_sqe();
        if (sqe == nullptr) {
            const int entered = ring_.enter(false);
            if (reap() == 0 && entered <= 0) std::this_thread::yield();
            continue;
        }
        IoOp& op = *waiting_head_;
        unlink_waiting(op);
        uring::prep_cancel(sqe, &op);
        uring::sqe_set_data(sqe, CANCEL_TAG);
        ++in_ring_;
    }
}

void IoUringBackend::ring_loop() noexcept {
    if (should_enable_ring_) {
        // Past a failure start() joins this thread and reports it.
        const bool enabled = ring_.enable();
        const int error = errno;
        enable_result_.store(enabled ? 1 : -error, std::memory_order_release);
        if (!enabled) return;
    }
    for (;;) {
        ring_.flush_completions();
        const bool reaped = reap() != 0;
        take_incoming();
        if (cancel_requested_.load(std::memory_order_seq_cst)) {
            cancel_requested_.store(false, std::memory_order_seq_cst);
            cancel_waiting();
        }
        fill_ring();
        if (stopping_.load(std::memory_order_seq_cst)) break;

        // Each side writes its flag and then reads the other, so either a
        // flush sees the park and wakes the ring or the ring sees the op.
        parked_.store(true, std::memory_order_seq_cst);
        // A full SQ left ops in the backlog: this enter makes room, so go round
        // again for them unless the CQ bound is what holds them.
        const bool backlog_can_move =
            backlog_head_ != nullptr && in_ring_ < ring_.cq_entries();
        const bool wait =
            !reaped && !backlog_can_move &&
            incoming_.load(std::memory_order_seq_cst) == nullptr &&
            !cancel_requested_.load(std::memory_order_seq_cst) &&
            !stopping_.load(std::memory_order_seq_cst) && !ring_.has_cqe();
        if (!wait) parked_.store(false, std::memory_order_relaxed);
        const int entered = ring_.enter(wait);
        parked_.store(false, std::memory_order_relaxed);
        if (entered == -EAGAIN || entered == -EBUSY) {
            // Out of kernel resources until completions drain. No wake comes
            // for that, so this retries.
            std::this_thread::yield();
        } else if (entered < 0 && entered != -EINTR) {
            DFTRACER_UTILS_LOG_ERROR("io_uring_enter failed: {}",
                                     std::strerror(-entered));
            std::this_thread::yield();
        }
    }
    // The executor saw zero in flight before stop(). Nothing here has reached
    // the kernel, so fail what a late submit left.
    take_incoming();
    while (backlog_head_ != nullptr) {
        IoOp& op = *backlog_head_;
        backlog_head_ = op.next;
        fail(op, -ECANCELED);
    }
    backlog_tail_ = nullptr;
    waiting_head_ = nullptr;
    in_ring_ = 0;
    wake_armed_ = false;
}

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_HAVE_IO_URING
