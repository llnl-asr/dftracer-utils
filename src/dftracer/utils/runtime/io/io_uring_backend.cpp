// SPDX-License-Identifier: MIT
#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/runtime/io/io_uring_backend.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>

namespace dftracer::utils::runtime::io {

// ============================================================================
// Ring implementation
// ============================================================================

namespace uring {

bool Ring::init(unsigned entries) {
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));

    ring_fd_ = sys_io_uring_setup(entries, &params);
    if (ring_fd_ < 0) {
        return false;
    }
    features_ = params.features;
    cq_entries_ = params.cq_entries;
    pending_ = 0;

    // Map SQ ring
    sq_ring_size_ = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    sq_ring_ptr_ =
        ::mmap(nullptr, sq_ring_size_, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
    if (sq_ring_ptr_ == MAP_FAILED) {
        ::close(ring_fd_);
        ring_fd_ = -1;
        return false;
    }

    auto* sq_base = static_cast<char*>(sq_ring_ptr_);
    sq_head_ = reinterpret_cast<unsigned*>(sq_base + params.sq_off.head);
    sq_tail_ = reinterpret_cast<unsigned*>(sq_base + params.sq_off.tail);
    sq_ring_mask_ =
        reinterpret_cast<unsigned*>(sq_base + params.sq_off.ring_mask);
    sq_ring_entries_ =
        reinterpret_cast<unsigned*>(sq_base + params.sq_off.ring_entries);
    sq_array_ = reinterpret_cast<unsigned*>(sq_base + params.sq_off.array);

    // Map SQEs
    sqes_size_ = params.sq_entries * sizeof(struct io_uring_sqe);
    sqes_ = static_cast<struct io_uring_sqe*>(
        ::mmap(nullptr, sqes_size_, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES));
    if (sqes_ == MAP_FAILED) {
        ::munmap(sq_ring_ptr_, sq_ring_size_);
        ::close(ring_fd_);
        sq_ring_ptr_ = nullptr;
        sqes_ = nullptr;
        ring_fd_ = -1;
        return false;
    }

    // Map CQ ring
    cq_ring_size_ =
        params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    cq_ring_ptr_ =
        ::mmap(nullptr, cq_ring_size_, PROT_READ | PROT_WRITE,
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

    auto* cq_base = static_cast<char*>(cq_ring_ptr_);
    cq_head_ = reinterpret_cast<unsigned*>(cq_base + params.cq_off.head);
    cq_tail_ = reinterpret_cast<unsigned*>(cq_base + params.cq_off.tail);
    cq_ring_mask_ =
        reinterpret_cast<unsigned*>(cq_base + params.cq_off.ring_mask);
    cqes_ =
        reinterpret_cast<struct io_uring_cqe*>(cq_base + params.cq_off.cqes);

    return true;
}

void Ring::destroy() {
    if (cq_ring_ptr_ && cq_ring_ptr_ != MAP_FAILED) {
        ::munmap(cq_ring_ptr_, cq_ring_size_);
        cq_ring_ptr_ = nullptr;
    }
    if (sqes_ && sqes_ != MAP_FAILED) {
        ::munmap(sqes_, sqes_size_);
        sqes_ = nullptr;
    }
    if (sq_ring_ptr_ && sq_ring_ptr_ != MAP_FAILED) {
        ::munmap(sq_ring_ptr_, sq_ring_size_);
        sq_ring_ptr_ = nullptr;
    }
    if (ring_fd_ >= 0) {
        ::close(ring_fd_);
        ring_fd_ = -1;
    }
}

struct io_uring_sqe* Ring::get_sqe() {
    const unsigned head =
        std::atomic_ref<unsigned>(*sq_head_).load(std::memory_order_acquire);
    const unsigned tail = *sq_tail_;
    if (tail - head >= *sq_ring_entries_) return nullptr;
    const unsigned index = tail & *sq_ring_mask_;
    sq_array_[index] = index;
    std::atomic_ref<unsigned>(*sq_tail_).store(tail + 1,
                                               std::memory_order_release);
    ++pending_;
    return &sqes_[index];
}

int Ring::enter(bool wait) {
    if (pending_ == 0 && !wait) return 0;
    const int taken = sys_io_uring_enter(ring_fd_, pending_, wait ? 1 : 0,
                                         wait ? IORING_ENTER_GETEVENTS : 0);
    if (taken < 0) return -errno;
    pending_ -= static_cast<unsigned>(taken);
    return taken;
}

struct io_uring_cqe* Ring::peek_cqe() {
    const unsigned head = *cq_head_;
    const unsigned tail =
        std::atomic_ref<unsigned>(*cq_tail_).load(std::memory_order_acquire);
    return head == tail ? nullptr : &cqes_[head & *cq_ring_mask_];
}

void Ring::cqe_seen() {
    std::atomic_ref<unsigned>(*cq_head_).store(*cq_head_ + 1,
                                               std::memory_order_release);
}

}  // namespace uring

// ============================================================================
// IoUringBackend implementation
// ============================================================================

namespace {

constexpr unsigned RING_ENTRIES = 256;

// Values of user_data that name no operation, which an IoOp address never is.
void* const WAKE_TAG = reinterpret_cast<void*>(std::uintptr_t{1});
void* const CANCEL_TAG = reinterpret_cast<void*>(std::uintptr_t{2});

// The most bytes one read or write moves, as read(2) and write(2) define it.
constexpr std::size_t MAX_RW_BYTES = 0x7ffff000;

unsigned rw_length(const IoOp& op) noexcept {
    return static_cast<unsigned>(std::min(op.length, MAX_RW_BYTES));
}

bool has_opcode(IoOpKind kind) noexcept {
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
        case IoOpKind::Fstat:
        case IoOpKind::Lseek:
        case IoOpKind::Sendfile:
        case IoOpKind::Call:
            return false;
    }
    return false;
}

bool can_wait_for_peer(IoOpKind kind) noexcept {
    switch (kind) {
        case IoOpKind::Read:
        case IoOpKind::Readv:
        case IoOpKind::Recv:
        case IoOpKind::Accept:
        case IoOpKind::Send:
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
        case IoOpKind::Fstat:
        case IoOpKind::Lseek:
        case IoOpKind::Sendfile:
        case IoOpKind::Call:
            break;
    }
    uring::sqe_set_data(sqe, &op);
}

}  // namespace

IoUringBackend::IoUringBackend(std::size_t pool_size,
                               IoPoolOptions pool_options)
    : ThreadPoolFileOps(pool_size, 0, pool_options) {}

IoUringBackend::~IoUringBackend() = default;

bool IoUringBackend::probe() {
    uring::Ring test_ring;
    // The ring runs sequential reads and writes at the file position.
    return test_ring.init(32) &&
           (test_ring.features() & IORING_FEAT_RW_CUR_POS) != 0;
}

void IoUringBackend::start() {
    cancelling_.store(false, std::memory_order_release);
    cancel_requested_.store(false, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    if (!ring_.init(RING_ENTRIES)) {
        throw common::DFTUtilsException(
            common::ErrorCode::Internal,
            common::str_cat("io_uring setup failed: ", std::strerror(errno)));
    }
    // Blocking, because the ring gets -EAGAIN from a non-blocking eventfd
    // instead of waiting for it.
    wake_fd_.reset(::eventfd(0, EFD_CLOEXEC));
    if (!wake_fd_.valid()) {
        const int error = errno;
        ring_.destroy();
        throw common::DFTUtilsException(
            common::ErrorCode::Internal,
            common::str_cat("eventfd failed: ", std::strerror(error)));
    }
    pool_.start();
    thread_ = dstd::Thread([this] { ring_loop(); });
}

void IoUringBackend::stop() {
    stopping_.store(true, std::memory_order_seq_cst);
    wake();
    if (thread_.joinable()) thread_.join();
    pool_.stop();
    ring_.destroy();
    wake_fd_.reset();
}

void IoUringBackend::submit(IoOp& op) {
    if (!has_opcode(op.kind)) {
        ThreadPoolFileOps::submit(op);
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
    if (incoming_.load(std::memory_order_seq_cst) != nullptr &&
        parked_.load(std::memory_order_seq_cst) &&
        parked_.exchange(false, std::memory_order_seq_cst)) {
        wake();
    }
    return woken;
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
    for (;;) {
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
            !stopping_.load(std::memory_order_seq_cst) &&
            ring_.peek_cqe() == nullptr;
        if (!wait) parked_.store(false, std::memory_order_relaxed);
        const int entered = ring_.enter(wait);
        parked_.store(false, std::memory_order_relaxed);
        if (entered == -EAGAIN || entered == -EBUSY) {
            // Out of kernel resources until completions drain. No wake comes
            // for that, so this retries.
            std::this_thread::yield();
        } else if (entered < 0 && entered != -EINTR) {
            DFTRACER_UTILS_LOG_ERROR("io_uring_enter failed: %s",
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
