// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/async_event.h>
#include <dftracer/utils/coro/detail/wake_batch.h>
#include <dftracer/utils/coro/executor.h>

namespace dftracer::utils::coro {

AsyncEvent::WaitAwaitable::~WaitAwaitable() {
    if (park_) executor_->unpark(park_);
    if (!queued_) return;
    std::lock_guard<std::mutex> lock(event_.mutex_);
    event_.remove_locked(this);
}

bool AsyncEvent::WaitAwaitable::suspend(std::coroutine_handle<> h,
                                        Executor* executor) {
    std::lock_guard<std::mutex> lock(event_.mutex_);
    if (event_.is_set()) return false;
    handle_ = h;
    executor_ = executor;
    next_ = nullptr;
    if (event_.tail_) {
        event_.tail_->next_ = this;
    } else {
        event_.head_ = this;
    }
    event_.tail_ = this;
    queued_ = true;
    if (executor_) park_ = executor_->park(this, &cancel_thunk);
    return true;
}

void AsyncEvent::WaitAwaitable::await_resume() {
    if (park_) {
        executor_->unpark(park_);
        park_ = nullptr;
    }
    queued_ = false;
    if (cancelled_) detail::cancelled_unstoppable("event wait");
}

void AsyncEvent::WaitAwaitable::cancel_thunk(void* self) noexcept {
    auto* waiter = static_cast<WaitAwaitable*>(self);
    {
        std::unique_lock<std::mutex> lock(waiter->event_.mutex_,
                                          std::try_to_lock);
        if (!lock || !waiter->event_.remove_locked(waiter)) return;
        waiter->cancelled_ = true;
    }
    detail::stop_or_resume(waiter->executor_, waiter->park_, waiter->handle_);
}

void AsyncEvent::set() {
    detail::WakeBatch wakes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_.store(true, std::memory_order_release);
        for (WaitAwaitable* waiter = head_; waiter;) {
            WaitAwaitable* next = waiter->next_;
            wakes.add(waiter->handle_, waiter->executor_);
            waiter->next_ = nullptr;
            waiter = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
    }
    wakes.post();
}

void AsyncEvent::reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    set_.store(false, std::memory_order_release);
}

bool AsyncEvent::remove_locked(WaitAwaitable* waiter) noexcept {
    WaitAwaitable* prev = nullptr;
    for (WaitAwaitable* curr = head_; curr; prev = curr, curr = curr->next_) {
        if (curr != waiter) continue;
        if (prev) {
            prev->next_ = curr->next_;
        } else {
            head_ = curr->next_;
        }
        if (tail_ == curr) tail_ = prev;
        curr->next_ = nullptr;
        return true;
    }
    return false;
}

}  // namespace dftracer::utils::coro
