#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/store/writer_extents.h>

#include <algorithm>

namespace dftracer::utils::store::engine {

WriterExtents::WriterExtents(std::uint64_t quota_bytes,
                             memory::Pool& pool) noexcept
    : pool_(&pool), quota_(quota_bytes) {}

WriterExtents::~WriterExtents() { reservation_.release(); }

bool WriterExtents::grow(std::uint64_t bytes) noexcept {
    if (reservation_) return reservation_.try_grow(bytes) == memory::Status::Ok;
    reservation_ = pool_->try_reserve(bytes);
    return static_cast<bool>(reservation_);
}

void WriterExtents::grow_forced(std::uint64_t bytes) noexcept {
    if (reservation_) {
        reservation_.grow(bytes);
        return;
    }
    reservation_ = pool_->reserve(bytes);
}

void WriterExtents::charge(std::uint64_t bytes) noexcept {
    held_ += bytes;
    if (held_ > peak_held_) peak_held_ = held_;
}

WriterExtents::Take WriterExtents::take(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (held_ + bytes <= quota_ && grow(bytes)) {
        charge(bytes);
        return Take::Taken;
    }
    if (held_ != 0) {
        ++busy_count_;
        return Take::Busy;
    }
    // Nothing is held, so no write can free room: charge past the quota and ask
    // the pool anyway so a manager under pressure sees the request.
    grow_forced(bytes);
    charge(bytes);
    ++over_quota_count_;
    return Take::TakenOverQuota;
}

void WriterExtents::release(std::uint64_t bytes) noexcept {
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

void WriterExtents::latch() noexcept {
    if (latched_.exchange(true, std::memory_order_acq_rel)) return;
    dstd::Vector<Waiter> waiters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters.swap(waiters_);
    }
    wake(waiters);
}

void WriterExtents::nudge() noexcept {
    dstd::Vector<Waiter> waiters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++epoch_;
        waiters.swap(waiters_);
    }
    wake(waiters);
}

void WriterExtents::wake(dstd::Vector<Waiter>& waiters) noexcept {
    for (const Waiter& waiter : waiters) {
        coro::resume_continuation(waiter.executor, waiter.handle);
    }
}

bool WriterExtents::ready_locked(std::uint64_t epoch) const noexcept {
    return latched_.load(std::memory_order_acquire) || held_ == 0 ||
           epoch_ != epoch;
}

bool WriterExtents::SpaceWait::await_ready() const {
    std::lock_guard<std::mutex> lock(extents_->mutex_);
    return extents_->ready_locked(epoch_);
}

bool WriterExtents::SpaceWait::await_suspend(std::coroutine_handle<> handle) {
    // Read before the lock: a handle parked with no executor can never resume.
    Executor* executor = Executor::current();
    std::lock_guard<std::mutex> lock(extents_->mutex_);
    if (extents_->ready_locked(epoch_)) return false;
    extents_->waiters_.push_back(Waiter{handle, executor});
    parked_ = handle;
    executor_ = executor;
    if (executor_ != nullptr) {
        park_ = executor_->park(this, &SpaceWait::ignore_cancel);
    }
    return true;
}

void WriterExtents::SpaceWait::await_resume() noexcept {
    if (park_ != nullptr) {
        executor_->unpark(park_);
        park_ = nullptr;
    }
    parked_ = nullptr;
}

WriterExtents::SpaceWait::~SpaceWait() {
    if (park_ != nullptr) executor_->unpark(park_);
    if (!parked_) return;
    std::lock_guard<std::mutex> lock(extents_->mutex_);
    auto& waiters = extents_->waiters_;
    for (std::size_t i = 0; i < waiters.size(); ++i) {
        if (waiters[i].handle == parked_) {
            waiters.erase(waiters.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

WriterExtents::WaitAnnounce::WaitAnnounce(WriterExtents& extents) noexcept
    : extents_(&extents) {
    extents_->waits_.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::lock_guard<std::mutex> lock(extents_->mutex_);
    epoch_ = extents_->epoch_;
}

WriterExtents::WaitAnnounce::~WaitAnnounce() {
    extents_->waits_.fetch_sub(1, std::memory_order_relaxed);
}

bool WriterExtents::waiting() const noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return waits_.load(std::memory_order_relaxed) != 0;
}

WriterExtentsCounters WriterExtents::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WriterExtentsCounters out;
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

}  // namespace dftracer::utils::store::engine
