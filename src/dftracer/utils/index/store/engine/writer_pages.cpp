#include <dftracer/utils/index/store/engine/writer_pages.h>
#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/runtime/executor/executor.h>

#include <algorithm>

namespace dftracer::utils::index::store::engine {

WriterPages::WriterPages(std::uint64_t quota_bytes, memory::Pool& pool) noexcept
    : pool_(&pool), quota_(quota_bytes) {}

WriterPages::~WriterPages() { reservation_.release(); }

bool WriterPages::grow(std::uint64_t bytes) noexcept {
    if (reservation_) return reservation_.try_grow(bytes) == memory::Status::Ok;
    reservation_ = pool_->try_reserve(bytes);
    return static_cast<bool>(reservation_);
}

void WriterPages::grow_forced(std::uint64_t bytes) noexcept {
    if (reservation_) {
        reservation_.grow(bytes);
        return;
    }
    reservation_ = pool_->reserve(bytes);
}

void WriterPages::charge(std::uint64_t bytes) noexcept {
    held_ += bytes;
    if (held_ > peak_held_) peak_held_ = held_;
}

WriterPages::Take WriterPages::take(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (held_ + bytes <= quota_ && grow(bytes)) {
        charge(bytes);
        return Take::Taken;
    }
    if (held_ != 0) {
        ++busy_count_;
        return Take::Busy;
    }
    // Nothing is held, so no write can free room before this take returns:
    // charge it past the quota, and ask the pool for the bytes anyway so a
    // manager under pressure sees the request.
    grow_forced(bytes);
    charge(bytes);
    ++over_quota_count_;
    return Take::TakenOverQuota;
}

void WriterPages::release(std::uint64_t bytes) noexcept {
    dstd::Vector<Waiter> waiters;
    memory::Reservation freed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytes > held_) {
            over_released_ += bytes - held_;
            bytes = held_;
        }
        if (bytes != 0) {
            held_ -= bytes;
            ++epoch_;
            const std::uint64_t charged = std::min(
                bytes, static_cast<std::uint64_t>(reservation_.bytes()));
            if (charged != 0) freed.take_from(reservation_, charged);
            waiters.swap(waiters_);
        }
    }
    // Past the lock: the bytes leave the pool here, and a waiter is resumed on
    // its own executor rather than on the thread that completed the write.
    freed.release();
    wake(waiters);
}

void WriterPages::latch() noexcept {
    if (latched_.exchange(true, std::memory_order_acq_rel)) return;
    dstd::Vector<Waiter> waiters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters.swap(waiters_);
    }
    wake(waiters);
}

void WriterPages::nudge() noexcept {
    dstd::Vector<Waiter> waiters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++epoch_;
        waiters.swap(waiters_);
    }
    wake(waiters);
}

void WriterPages::wake(dstd::Vector<Waiter>& waiters) noexcept {
    for (const Waiter& waiter : waiters) {
        coro::resume_continuation(waiter.executor, waiter.handle);
    }
}

bool WriterPages::ready_locked(std::uint64_t epoch) const noexcept {
    return latched_.load(std::memory_order_acquire) || held_ == 0 ||
           epoch_ != epoch;
}

bool WriterPages::SpaceWait::await_ready() const {
    std::lock_guard<std::mutex> lock(pages_->mutex_);
    return pages_->ready_locked(epoch_);
}

bool WriterPages::SpaceWait::await_suspend(std::coroutine_handle<> handle) {
    // Read before the lock: a handle parked with no executor can never resume.
    Executor* executor = Executor::current();
    std::lock_guard<std::mutex> lock(pages_->mutex_);
    if (pages_->ready_locked(epoch_)) return false;
    pages_->waiters_.push_back(Waiter{handle, executor});
    parked_ = handle;
    return true;
}

WriterPages::SpaceWait::~SpaceWait() {
    if (!parked_) return;
    std::lock_guard<std::mutex> lock(pages_->mutex_);
    auto& waiters = pages_->waiters_;
    for (std::size_t i = 0; i < waiters.size(); ++i) {
        if (waiters[i].handle == parked_) {
            waiters.erase(waiters.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

WriterPages::WaitAnnounce::WaitAnnounce(WriterPages& pages) noexcept
    : pages_(&pages) {
    pages_->waits_.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::lock_guard<std::mutex> lock(pages_->mutex_);
    epoch_ = pages_->epoch_;
}

WriterPages::WaitAnnounce::~WaitAnnounce() {
    pages_->waits_.fetch_sub(1, std::memory_order_relaxed);
}

bool WriterPages::waiting() const noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return waits_.load(std::memory_order_relaxed) != 0;
}

WriterPagesCounters WriterPages::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WriterPagesCounters out;
    out.quota = quota_;
    out.held = held_;
    out.peak_held = peak_held_;
    out.busy = busy_count_;
    out.over_quota = over_quota_count_;
    out.over_released = over_released_;
    out.reservation = static_cast<std::uint64_t>(reservation_.bytes());
    out.reserved =
        pool_ != nullptr ? static_cast<std::uint64_t>(pool_->reserved()) : 0;
    return out;
}

}  // namespace dftracer::utils::index::store::engine
