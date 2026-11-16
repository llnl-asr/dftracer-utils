// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/op.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm {

namespace {

/// Calls that are alive now, which the tests read to prove a finished call
/// pins nothing.
std::atomic<std::size_t> g_live_ops{0};

}  // namespace

namespace detail {

std::size_t count_live_ops() noexcept {
    return g_live_ops.load(std::memory_order_acquire);
}

}  // namespace detail

PendingOp::PendingOp() noexcept {
    g_live_ops.fetch_add(1, std::memory_order_relaxed);
}

PendingOp::~PendingOp() { g_live_ops.fetch_sub(1, std::memory_order_relaxed); }

void PendingOp::wait_done() {
    coro::BlockingRegion region{};
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return done_.load(std::memory_order_acquire); });
}

void PendingOp::fail(dstd::Error why) noexcept {
    this->settle([this, &why] { error_ = std::move(why); });
}

bool PendingOp::park(std::coroutine_handle<> handle, coro::Executor* executor,
                     void* awaiter, coro::detail::ParkCancelFn cancel,
                     coro::detail::ParkNode*& node) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.load(std::memory_order_relaxed)) return false;
    waiters_.push_back(detail::OpWaiter{handle, executor});
    // Parked under the lock: the resume of this call takes the same lock, so a
    // wake that lands here finds the entry.
    if (executor != nullptr) node = executor->park(awaiter, cancel);
    return true;
}

bool PendingOp::withdraw(std::coroutine_handle<> handle) noexcept {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return false;
    if (done_.load(std::memory_order_relaxed)) return false;
    for (auto at = waiters_.begin(); at != waiters_.end(); ++at) {
        if (at->handle != handle) continue;
        waiters_.erase(at);
        return true;
    }
    return false;
}

void PendingOp::set_timeout(
    coro::TimerService* timers, coro::TimerService::TimerId id,
    dstd::SharedPtr<detail::TimerTarget> target) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_.load(std::memory_order_relaxed)) {
            timer_target_ = std::move(target);
            timers_ = timers;
            timer_ = id;
            return;
        }
    }
    // The call ended before the timeout was noted, so nothing waits on it.
    if (timers != nullptr && id != coro::TimerService::NO_TIMER)
        timers->cancel_timeout(id);
    if (target) target->clear();
}

void PendingOp::end_timeout() noexcept {
    coro::TimerService* timers = nullptr;
    coro::TimerService::TimerId id = coro::TimerService::NO_TIMER;
    dstd::SharedPtr<detail::TimerTarget> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timers = timers_;
        id = timer_;
        target = std::move(timer_target_);
        timers_ = nullptr;
        timer_ = coro::TimerService::NO_TIMER;
    }
    if (timers != nullptr && id != coro::TimerService::NO_TIMER)
        timers->cancel_timeout(id);
    // The timer entry keeps only @p target until its deadline: emptying it
    // frees this call and its group now, not at the deadline.
    if (target) target->clear();
}

}  // namespace dftracer::utils::comm
