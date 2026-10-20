#ifndef DFTRACER_UTILS_RUNTIME_CORO_COMPLETION_STATE_H
#define DFTRACER_UTILS_RUNTIME_CORO_COMPLETION_STATE_H

#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/parked.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>

namespace dftracer::utils {
class Executor;
namespace coro {

/// Common suspension/completion coordination shared by when_all / when_any
/// shared-state structs. Holds the awaiting coroutine, its executor, and the
/// CompletionLatch that guards the suspend-vs-complete race. While the awaiter
/// is suspended it is registered with the executor, so shutdown resumes it
/// with a cancelled error even when a child never completes.
struct CompletionState {
    std::coroutine_handle<> awaiting_coroutine_{};
    Executor* executor_{nullptr};
    CompletionLatch latch_;

    ~CompletionState() {
        if (park_) executor_->unpark(park_);
    }

    /// Called by await_suspend after deciding to suspend but before returning.
    void mark_suspended_and_check_completion() {
        // Children that are already done leave nothing to wait for, so the
        // registry is not touched.
        if (executor_ && !latch_.settled()) {
            park_ = executor_->park(this, &CompletionState::cancel_thunk);
        }
        const std::uint8_t before = latch_.on_suspended();
        if (before &
            (CompletionLatch::BIT_COMPLETED | CompletionLatch::BIT_CANCELLED)) {
            if (!(before & CompletionLatch::BIT_COMPLETED)) cancelled_ = true;
            resume_continuation(executor_, awaiting_coroutine_);
        }
    }

    /// First step of await_resume: leaves the registry and throws the
    /// cancelled error when shutdown, not completion, resumed the awaiter.
    void finish_wait() {
        if (park_) {
            executor_->unpark(park_);
            park_ = nullptr;
        }
        if (cancelled_) throw_cancelled("wait cancelled");
    }

    static void cancel_thunk(void* self) noexcept {
        static_cast<CompletionState*>(self)->cancel_parked();
    }

    void cancel_parked() noexcept {
        if (latch_.on_cancelled()) {
            cancelled_ = true;
            resume_continuation(executor_, awaiting_coroutine_);
        }
    }

   private:
    ParkNode* park_ = nullptr;
    bool cancelled_ = false;
};

/// All-of accounting: resumes only after every child completes, recording the
/// first exception observed.
struct WhenAllCompletionState : CompletionState {
    std::exception_ptr exception_;
    std::atomic<bool> has_exception_{false};
    std::atomic<std::size_t> completed_count_{0};
    std::size_t total_{0};

    void on_one_complete() {
        std::size_t count =
            completed_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == total_) {
            if (latch_.on_completed())
                resume_continuation(executor_, awaiting_coroutine_);
        }
    }

    void on_exception(std::exception_ptr e) {
        bool expected = false;
        if (has_exception_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
            exception_ = e;
        }
        on_one_complete();
    }
};

/// First-of accounting: the awaiter resumes once a winner is decided and every
/// child wrapper has finished, so no loser runs after the awaiter resumes.
/// One atomic holds both facts, so the one update that completes the condition
/// sees the other half and releases the awaiter, once, with no extra handshake.
struct WhenAnyCompletionState : CompletionState {
    std::size_t wrappers_total_{0};

    void on_first_complete() {
        const std::size_t before =
            progress_.fetch_or(WINNER, std::memory_order_acq_rel);
        if ((before & FINISHED_MASK) == wrappers_total_) release();
    }

    void on_wrapper_finished() {
        const std::size_t before =
            progress_.fetch_add(1, std::memory_order_acq_rel);
        if ((before & WINNER) != 0 &&
            (before & FINISHED_MASK) + 1 == wrappers_total_) {
            release();
        }
    }

    /// True once a winner is decided and every wrapper is done.
    bool released() const {
        const std::size_t now = progress_.load(std::memory_order_acquire);
        return (now & WINNER) != 0 && (now & FINISHED_MASK) == wrappers_total_;
    }

   private:
    static constexpr std::size_t WINNER = std::size_t{1}
                                          << (sizeof(std::size_t) * 8 - 1);
    static constexpr std::size_t FINISHED_MASK = ~WINNER;

    void release() {
        if (latch_.on_completed())
            resume_continuation(executor_, awaiting_coroutine_);
    }

    std::atomic<std::size_t> progress_{0};
};

}  // namespace coro
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_CORO_COMPLETION_STATE_H
