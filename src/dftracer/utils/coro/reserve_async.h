// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_RESERVE_ASYNC_H
#define DFTRACER_UTILS_CORO_RESERVE_ASYNC_H

#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/detail/pool_access.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

// Lives off the frame as the node's resume context. `self` keeps it alive while
// a grant may still call on_grant, and only the party that moves the node out
// of Waiting resets it. The timer holds it weakly. Resume is noexcept, so an
// enqueue that fails for lack of memory terminates.
struct ReserveWaitState {
    dstd::SharedPtr<ReserveWaitState> self;
    std::coroutine_handle<> handle;
    Executor* executor = nullptr;
    TimerService* timers = nullptr;
    memory::detail::ReserveWait* node = nullptr;
    std::atomic<TimerService::TimerId> timer{0};
    // Set once the node is listed. Before that, a shutdown cancel is skipped
    // and the next round finds the waiter.
    std::atomic<bool> listed{false};
    // Set when shutdown, not a deadline, ended the wait.
    std::atomic<bool> cancelled{false};

    ReserveWaitState(std::coroutine_handle<> h, Executor* exec) noexcept
        : handle(h),
          executor(exec),
          timers(exec->timers() != nullptr ? exec->timers()
                                           : &process_timers()) {}
    ReserveWaitState(const ReserveWaitState&) = delete;
    ReserveWaitState& operator=(const ReserveWaitState&) = delete;
    ~ReserveWaitState() {
        if (node != nullptr) node->drop();
    }

    static void on_grant(void* context) noexcept {
        auto* state = static_cast<ReserveWaitState*>(context);
        dstd::SharedPtr<ReserveWaitState> keep = std::move(state->self);
        state->executor->post(state->handle);
    }

    void on_timeout() {
        if (!node->cancel()) return;
        dstd::SharedPtr<ReserveWaitState> keep = std::move(self);
        executor->post(handle);
    }

    void on_cancel() {
        if (!node->cancel()) return;
        cancelled.store(true, std::memory_order_release);
        dstd::SharedPtr<ReserveWaitState> keep = std::move(self);
        executor->post(handle);
    }

    void cancel_timer() {
        TimerService::TimerId id = timer.exchange(0, std::memory_order_acq_rel);
        if (id != 0) timers->cancel_timeout(id);
    }
};

}  // namespace detail

/**
 * @brief The awaitable of reserve_async(). Await it once.
 * Yields the reservation, or the status when there is none: Cancelled at
 * executor shutdown, OverBudget when the request can never fit or the deadline
 * passed, else the Pool::reserve status when the call did not wait.
 */
class ReserveAsync {
   public:
    using Clock = std::chrono::steady_clock;
    using Result = dstd::Expected<memory::Reservation, memory::Status>;

    ReserveAsync(memory::Pool& pool, std::size_t bytes,
                 Clock::time_point deadline) noexcept
        : pool_(&pool), bytes_(bytes), deadline_(deadline) {}
    ReserveAsync(ReserveAsync&&) noexcept = default;
    ReserveAsync& operator=(ReserveAsync&&) = delete;
    ReserveAsync(const ReserveAsync&) = delete;
    ReserveAsync& operator=(const ReserveAsync&) = delete;

    // A frame destroyed while it waits cancels its node here, so no grant
    // and no timer resumes it later. One destroyed after a grant or a timeout
    // already won is resumed anyway, as with any awaitable.
    ~ReserveAsync() {
        if (park_) park_executor_->unpark(park_);
        if (!state_) return;
        if (state_->node->cancel()) state_->self.reset();
        state_->cancel_timer();
    }

    bool await_ready() noexcept {
        reserved_ = memory::detail::PoolAccess::reserve(*pool_, bytes_);
        if (reserved_) return true;
        Executor* exec = Executor::current();
        return exec == nullptr || !exec->runs_remote_wakeups() ||
               Clock::now() >= deadline_;
    }

    bool await_suspend(std::coroutine_handle<> h) {
        Executor* exec = Executor::current();
        const Clock::time_point deadline = deadline_;
        // On the general pool: the timer can keep it past the task's pool.
        auto state = std::allocate_shared<detail::ReserveWaitState>(
            memory::PoolAllocator<detail::ReserveWaitState>(
                memory::detail::Manager::get().general()),
            h, exec);
        state->node = memory::detail::ReserveWait::create(
            *pool_, bytes_, &detail::ReserveWaitState::on_grant, state.get());
        if (state->node == nullptr) return false;
        state_ = state;
        state->self = state;
        park_executor_ = exec;
        if (exec) park_ = exec->park(this, &ReserveAsync::cancel_thunk);
        // From the push on, a grant may resume the frame on another thread,
        // so only the local `state` is touched below.
        memory::Status pushed = state->node->push();
        if (pushed != memory::Status::Pending) {
            state->self.reset();
            if (park_) exec->unpark(park_);
            park_ = nullptr;
            return false;
        }
        state->listed.store(true, std::memory_order_release);
        if (deadline == Clock::time_point::max()) return true;
        TimerService& timers = *state->timers;
        timers.start();
        dstd::WeakPtr<detail::ReserveWaitState> weak = state;
        TimerService::TimerId id =
            timers.register_timeout(deadline - Clock::now(), [weak] {
                if (auto s = weak.lock()) s->on_timeout();
            });
        state->timer.store(id, std::memory_order_release);
        if (state->node->state() != memory::detail::WaitState::Waiting) {
            state->cancel_timer();
        }
        return true;
    }

    static void cancel_thunk(void* self) noexcept {
        static_cast<ReserveAsync*>(self)->cancel_parked();
    }

    void cancel_parked() noexcept {
        if (state_ && state_->listed.load(std::memory_order_acquire)) {
            state_->on_cancel();
        }
    }

    Result await_resume() {
        if (park_) {
            park_executor_->unpark(park_);
            park_ = nullptr;
        }
        if (reserved_) return std::move(reserved_);
        if (!state_) return dstd::Unexpected(reserved_.status());
        state_->cancel_timer();
        memory::Reservation granted = state_->node->take();
        if (granted) return granted;
        return dstd::Unexpected(
            state_->cancelled.load(std::memory_order_acquire)
                ? memory::Status::Cancelled
                : memory::Status::OverBudget);
    }

   private:
    dstd::SharedPtr<detail::ReserveWaitState> state_;
    Executor* park_executor_ = nullptr;
    detail::ParkNode* park_ = nullptr;
    memory::Pool* pool_;
    std::size_t bytes_;
    Clock::time_point deadline_;
    memory::Reservation reserved_;
};

/**
 * Reserves @p bytes in @p pool, waiting for a release until @p deadline
 * (Clock::time_point::max() means none). Resumes on its executor, never inline
 * on the releasing thread. Does not wait off an executor or past the deadline.
 * The pool must stay open until the reservation is released.
 */
inline ReserveAsync reserve_async(memory::Pool& pool, std::size_t bytes,
                                  ReserveAsync::Clock::time_point deadline) {
    return ReserveAsync(pool, bytes, deadline);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_RESERVE_ASYNC_H
