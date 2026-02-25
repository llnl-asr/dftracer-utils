#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include "io_uring_backend.h"

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <fcntl.h>
#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>

namespace dftracer::utils::io {

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
    cq_ring_entries_ =
        reinterpret_cast<unsigned*>(cq_base + params.cq_off.ring_entries);
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
    unsigned head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    unsigned tail = *sq_tail_;
    unsigned mask = *sq_ring_mask_;

    if (tail - head >= *sq_ring_entries_) {
        return nullptr;  // SQ is full
    }

    unsigned index = tail & mask;
    sq_array_[index] = index;
    auto* sqe = &sqes_[index];
    *sq_tail_ = tail + 1;
    return sqe;
}

int Ring::submit() {
    // Legacy single-submit path. Used only by stop() for NOP wakeup.
    // For batched submission, use mark_pending() + flush().
    pending_count_ = 1;
    return flush();
}

int Ring::flush() {
    if (pending_count_ == 0) return 0;
    unsigned to_submit = pending_count_;
    pending_count_ = 0;
    // Memory barrier: make SQE writes visible to kernel
    __atomic_store_n(sq_tail_, *sq_tail_, __ATOMIC_RELEASE);
    return sys_io_uring_enter(ring_fd_, to_submit, 0, 0);
}

struct io_uring_cqe* Ring::peek_cqe() {
    unsigned head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);

    if (head == tail) {
        return nullptr;  // CQ is empty
    }

    unsigned mask = *cq_ring_mask_;
    return &cqes_[head & mask];
}

int Ring::wait_cqe(struct io_uring_cqe** cqe_out) {
    // First try non-blocking peek
    auto* cqe = peek_cqe();
    if (cqe) {
        *cqe_out = cqe;
        return 0;
    }

    // Block until at least one CQE is available
    int ret = sys_io_uring_enter(ring_fd_, 0, 1, IORING_ENTER_GETEVENTS);
    if (ret < 0) {
        return -errno;
    }

    cqe = peek_cqe();
    if (!cqe) {
        return -EAGAIN;
    }
    *cqe_out = cqe;
    return 0;
}

void Ring::cqe_seen(struct io_uring_cqe* /*cqe*/) {
    unsigned head = *cq_head_;
    __atomic_store_n(cq_head_, head + 1, __ATOMIC_RELEASE);
}

}  // namespace uring

// ============================================================================
// IoUringBackend implementation
// ============================================================================

IoUringBackend::IoUringBackend(Executor& executor, unsigned ring_entries,
                               unsigned batch_threshold)
    : executor_(executor),
      ring_entries_(ring_entries),
      batch_threshold_(batch_threshold) {}

bool IoUringBackend::probe() {
    uring::Ring test_ring;
    return test_ring.init(32);
}

void IoUringBackend::start() {
    if (!ring_.init(ring_entries_)) {
        DFTRACER_UTILS_LOG_ERROR("io_uring ring init failed (entries=%u)",
                                 ring_entries_);
        return;
    }
    DFTRACER_UTILS_LOG_DEBUG(
        "io_uring backend started (ring entries=%u, batch_threshold=%u)",
        ring_entries_, batch_threshold_);

    completion_thread_.start([this] { completion_loop(); });
}

void IoUringBackend::stop() {
    // 1. Signal the completion thread to stop (sets running_ = false).
    completion_thread_.signal_stop();

    // 2. Submit a NOP to wake the thread blocked on wait_cqe.
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        auto* sqe = ring_.get_sqe();
        if (sqe) {
            uring::prep_nop(sqe);
            uring::sqe_set_data(sqe, nullptr);
            // NOP bypass: submit immediately, don't batch
            ring_.mark_pending();
            ring_.flush();
        }
    }

    // 3. Join the thread (now it will see running_ == false after
    //    the NOP CQE wakes it from wait_cqe).
    completion_thread_.join();
    ring_.destroy();
}

void IoUringBackend::completion_loop() {
    while (completion_thread_.running()) {
        struct io_uring_cqe* cqe = nullptr;
        int ret = ring_.wait_cqe(&cqe);
        if (ret < 0) {
            // EINTR or shutdown -- just retry
            continue;
        }
        if (!cqe) {
            continue;
        }

        auto* req = static_cast<IoUringRequest*>(uring::cqe_get_data(cqe));
        if (req && req->awaitable) {
            req->awaitable->result_ = cqe->res;
            executor_.enqueue(req->awaitable->handle_);
            delete req;
        }
        ring_.cqe_seen(cqe);
    }
}

void IoUringBackend::maybe_flush_locked() {
    if (ring_.pending_count() >= batch_threshold_) {
        int ret = ring_.flush();
        if (ret < 0) {
            DFTRACER_UTILS_LOG_ERROR("io_uring flush failed: %s",
                                     std::strerror(-ret));
        }
    }
}

int IoUringBackend::flush() {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    int ret = ring_.flush();
    if (ret < 0) {
        DFTRACER_UTILS_LOG_ERROR("io_uring flush failed: %s",
                                 std::strerror(-ret));
    }
    return ret;
}

void IoUringBackend::submit_fn(SubmitContext* ctx, IoAwaitable* awaitable) {
    auto* uring_ctx = static_cast<IoUringSubmitCtx*>(ctx);
    auto* backend = uring_ctx->backend;

    // Create the request object that will be stored in SQE user_data
    auto* req = new IoUringRequest{};
    req->awaitable = awaitable;

    std::lock_guard<std::mutex> lock(backend->submit_mutex_);

    struct io_uring_sqe* sqe = backend->ring_.get_sqe();
    if (!sqe) {
        // SQ full -- fall back to synchronous execution
        ssize_t result = 0;
        switch (uring_ctx->op) {
            case IoUringSubmitCtx::Op::READ:
                result = ::pread(uring_ctx->fd, uring_ctx->buf, uring_ctx->len,
                                 uring_ctx->offset);
                break;
            case IoUringSubmitCtx::Op::WRITE:
                result = ::pwrite(uring_ctx->fd, uring_ctx->buf, uring_ctx->len,
                                  uring_ctx->offset);
                break;
            case IoUringSubmitCtx::Op::OPEN:
                result =
                    ::open(uring_ctx->path, uring_ctx->flags, uring_ctx->mode);
                break;
            case IoUringSubmitCtx::Op::CLOSE:
                result = ::close(uring_ctx->fd);
                break;
            case IoUringSubmitCtx::Op::FSYNC:
                result = ::fsync(uring_ctx->fd);
                break;
            case IoUringSubmitCtx::Op::FTRUNCATE:
                result = ::ftruncate(uring_ctx->fd, uring_ctx->offset);
                break;
            case IoUringSubmitCtx::Op::FSTAT:
                result = ::fstat(uring_ctx->fd, uring_ctx->stat_buf);
                break;
        }
        if (result < 0) result = -errno;
        awaitable->result_ = result;
        delete req;
        delete uring_ctx;
        backend->executor_.enqueue(awaitable->handle_);
        return;
    }

    switch (uring_ctx->op) {
        case IoUringSubmitCtx::Op::READ:
            uring::prep_read(sqe, uring_ctx->fd, uring_ctx->buf,
                             static_cast<unsigned>(uring_ctx->len),
                             uring_ctx->offset);
            break;
        case IoUringSubmitCtx::Op::WRITE:
            uring::prep_write(sqe, uring_ctx->fd, uring_ctx->buf,
                              static_cast<unsigned>(uring_ctx->len),
                              uring_ctx->offset);
            break;
        case IoUringSubmitCtx::Op::OPEN:
            uring::prep_openat(sqe, AT_FDCWD, uring_ctx->path, uring_ctx->flags,
                               uring_ctx->mode);
            break;
        case IoUringSubmitCtx::Op::CLOSE:
            uring::prep_close(sqe, uring_ctx->fd);
            break;
        case IoUringSubmitCtx::Op::FSYNC:
            uring::prep_fsync(sqe, uring_ctx->fd, 0);
            break;
        case IoUringSubmitCtx::Op::FTRUNCATE:
            // IORING_OP_FTRUNCATE requires kernel 6.9+. Fall back to
            // sync.
            {
                ssize_t sync_result =
                    ::ftruncate(uring_ctx->fd, uring_ctx->offset);
                if (sync_result < 0) sync_result = -errno;
                awaitable->result_ = sync_result;
                delete req;
                delete uring_ctx;
                backend->executor_.enqueue(awaitable->handle_);
                return;
            }
        case IoUringSubmitCtx::Op::FSTAT:
            // fstat is fast (cached inode). Use sync path for
            // simplicity.
            {
                ssize_t sync_result =
                    ::fstat(uring_ctx->fd, uring_ctx->stat_buf);
                if (sync_result < 0) sync_result = -errno;
                awaitable->result_ = sync_result;
                delete req;
                delete uring_ctx;
                backend->executor_.enqueue(awaitable->handle_);
                return;
            }
    }

    uring::sqe_set_data(sqe, req);
    backend->ring_.mark_pending();
    backend->maybe_flush_locked();
    delete uring_ctx;
}

static IoAwaitable make_uring_request(IoUringSubmitCtx::Op op, int fd,
                                      void* buf, std::size_t len, off_t offset,
                                      const char* path, int flags, mode_t mode,
                                      IoUringBackend* backend) {
    auto* ctx = new IoUringSubmitCtx{};
    ctx->submit = &IoUringBackend::submit_fn;
    ctx->op = op;
    ctx->fd = fd;
    ctx->buf = buf;
    ctx->len = len;
    ctx->offset = offset;
    ctx->path = path;
    ctx->flags = flags;
    ctx->mode = mode;
    ctx->backend = backend;

    IoAwaitable awaitable;
    awaitable.submit_ctx_ = ctx;
    return awaitable;
}

IoAwaitable IoUringBackend::submit_read(int fd, void* buf, std::size_t len,
                                        off_t offset) {
    return make_uring_request(IoUringSubmitCtx::Op::READ, fd, buf, len, offset,
                              nullptr, 0, 0, this);
}

IoAwaitable IoUringBackend::submit_write(int fd, const void* buf,
                                         std::size_t len, off_t offset) {
    return make_uring_request(IoUringSubmitCtx::Op::WRITE, fd,
                              const_cast<void*>(buf), len, offset, nullptr, 0,
                              0, this);
}

IoAwaitable IoUringBackend::submit_open(const char* path, int flags,
                                        mode_t mode) {
    return make_uring_request(IoUringSubmitCtx::Op::OPEN, -1, nullptr, 0, 0,
                              path, flags, mode, this);
}

IoAwaitable IoUringBackend::submit_close(int fd) {
    return make_uring_request(IoUringSubmitCtx::Op::CLOSE, fd, nullptr, 0, 0,
                              nullptr, 0, 0, this);
}

IoAwaitable IoUringBackend::submit_fsync(int fd) {
    return make_uring_request(IoUringSubmitCtx::Op::FSYNC, fd, nullptr, 0, 0,
                              nullptr, 0, 0, this);
}

IoAwaitable IoUringBackend::submit_ftruncate(int fd, off_t length) {
    return make_uring_request(IoUringSubmitCtx::Op::FTRUNCATE, fd, nullptr, 0,
                              length, nullptr, 0, 0, this);
}

IoAwaitable IoUringBackend::submit_fstat(int fd, struct stat* buf) {
    auto ctx = make_uring_request(IoUringSubmitCtx::Op::FSTAT, fd, nullptr, 0,
                                  0, nullptr, 0, 0, this);
    auto* uring_ctx = static_cast<IoUringSubmitCtx*>(ctx.submit_ctx_);
    uring_ctx->stat_buf = buf;
    return ctx;
}

std::size_t IoUringBackend::poll(int /*timeout_ms*/) {
    // The dedicated completion thread is the sole CQ consumer.
    // Workers must NOT read CQEs -- doing so would race with
    // completion_loop() and cause use-after-free.
    return 0;
}

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_HAVE_IO_URING
