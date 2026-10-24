#ifndef DFTRACER_UTILS_RUNTIME_CORO_RESERVE_ASYNC_H
#define DFTRACER_UTILS_RUNTIME_CORO_RESERVE_ASYNC_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/parked.h>
#include <dftracer/utils/runtime/executor/timer_service.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

// Lives off the frame because it is the node's resume context. `self` keeps
// it alive while a grant may still call on_grant; only the party that moves
// the node out of Waiting resets it. The timer holds it weakly, so a cancelled
// timer that waits for its expiry keeps nothing alive.
// Resume is noexcept: an enqueue that fails for lack of memory terminates.
struct ReserveWaitState {
    std::shared_ptr<ReserveWaitState> self;
    std::coroutine_handle<> handle;
    Executor* executor = nullptr;
    TimerService* timers = nullptr;
    memory::ReserveWait* node = nullptr;
    std::atomic<TimerService::TimerId> timer{0};
    // Set once the node is listed; before that, a shutdown cancel is skipped
    // and the next round finds the waiter.
    std::atomic<bool> listed{false};
    // Set when shutdown, not a deadline, ended the wait.
    std::atomic<bool> cancelled{false};

    ReserveWaitState(std::coroutine_handle<> h, Executor* exec) noexcept
        : handle(h), executor(exec), timers(&exec->get_timer_service()) {}
    ReserveWaitState(const ReserveWaitState&) = delete;
    ReserveWaitState& operator=(const ReserveWaitState&) = delete;
    ~ReserveWaitState() {
        if (node != nullptr) node->drop();
    }

    static void on_grant(void* context) noexcept {
        auto* state = static_cast<ReserveWaitState*>(context);
        std::shared_ptr<ReserveWaitState> keep = std::move(state->self);
        state->executor->enqueue(state->handle);
    }

    void on_timeout() {
        if (!node->cancel()) return;
        std::shared_ptr<ReserveWaitState> keep = std::move(self);
        executor->enqueue(handle);
    }

    void on_cancel() {
        if (!node->cancel()) return;
        cancelled.store(true, std::memory_order_release);
        std::shared_ptr<ReserveWaitState> keep = std::move(self);
        executor->enqueue(handle);
    }

    void cancel_timer() {
        TimerService::TimerId id = timer.exchange(0, std::memory_order_acq_rel);
        if (id != 0) timers->cancel_timeout(id);
    }
};

}  // namespace detail

/**
 * @brief The awaitable of reserve_async(); co_await it once.
 *
 * Yields the reservation, or the status when there is none: Status::Cancelled
 * when the executor shut down while it waited, Status::OverBudget when the
 * request can never fit or the deadline passed first, else the status of the
 * failed Pool::reserve when the call did not wait.
 */
class ReserveAsync {
   public:
    using Clock = std::chrono::steady_clock;
    using Result = expected<memory::Reservation, memory::Status>;

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
        reserved_ = pool_->reserve(bytes_);
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
                memory::Manager::get().general()),
            h, exec);
        state->node = memory::ReserveWait::create(
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
        try {
            TimerService& timers = *state->timers;
            timers.start();
            std::weak_ptr<detail::ReserveWaitState> weak = state;
            TimerService::TimerId id =
                timers.register_timeout(deadline - Clock::now(), [weak] {
                    if (auto s = weak.lock()) s->on_timeout();
                });
            state->timer.store(id, std::memory_order_release);
        } catch (...) {
            // A grant that already won owns the resume; else the frame
            // resumes here with the error.
            if (!state->node->cancel()) return true;
            state->self.reset();
            if (park_) exec->unpark(park_);
            park_ = nullptr;
            throw;
        }
        if (state->node->state() != memory::WaitState::Waiting) {
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
        if (!state_) return unexpected(reserved_.status());
        state_->cancel_timer();
        memory::Reservation granted = state_->node->take();
        if (granted) return granted;
        return unexpected(state_->cancelled.load(std::memory_order_acquire)
                              ? memory::Status::Cancelled
                              : memory::Status::OverBudget);
    }

   private:
    std::shared_ptr<detail::ReserveWaitState> state_;
    Executor* park_executor_ = nullptr;
    ParkNode* park_ = nullptr;
    memory::Pool* pool_;
    std::size_t bytes_;
    Clock::time_point deadline_;
    memory::Reservation reserved_;
};

/**
 * @brief Reserve @p bytes in @p pool, waiting until a release frees room or
 * @p deadline passes.
 *
 * Tries Pool::reserve first, which also asks holders to give bytes back. When
 * that fails on an executor thread, the coroutine waits in the pool's wait
 * list and resumes on the same executor, never inline on the releasing
 * thread. Off an executor, on one that does not run remote wake-ups (the
 * RunLoop of a blocking get()), or with the deadline already past, it does
 * not wait. Clock::time_point::max() waits with no deadline. The pool must stay
 * open until the coroutine resumes and its reservation is released.
 */
inline ReserveAsync reserve_async(memory::Pool& pool, std::size_t bytes,
                                  ReserveAsync::Clock::time_point deadline) {
    return ReserveAsync(pool, bytes, deadline);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_RESERVE_ASYNC_H
