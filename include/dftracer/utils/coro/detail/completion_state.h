// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_COMPLETION_STATE_H
#define DFTRACER_UTILS_CORO_DETAIL_COMPLETION_STATE_H

#include <dftracer/utils/coro/completion_latch.h>
#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/coro/executor.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>

namespace dftracer::utils::coro::detail {

/// Common suspension/completion coordination shared by when_all / when_any
/// shared-state structs. Holds the awaiting coroutine, its executor, and the
/// @ref CompletionLatch that guards the suspend-vs-complete race. A child that
/// stopped and no error end the awaiter as stopped, in place of resuming it.
/// Shutdown ends a suspended awaiter the same way.
struct CompletionState {
    std::coroutine_handle<> awaiting_coroutine{};
    Executor* executor{nullptr};
    ParkOwner owner;
    CompletionLatch latch;
    /// A child stopped. Set before the child counts as done.
    std::atomic<bool> stopped{false};
    /// A child failed with an exception, which the awaiter raises in place of
    /// the stop.
    std::atomic<bool> failed{false};

    /// Records the awaiter. Call first in await_suspend.
    template <typename Promise>
    void bind_awaiter(std::coroutine_handle<Promise> h) noexcept {
        awaiting_coroutine = h;
        owner = park_owner_of(h);
    }

    bool stop_wanted() const noexcept {
        return stopped.load(std::memory_order_acquire) &&
               !failed.load(std::memory_order_acquire);
    }

    /// For an await_suspend that finds its children done. True when the awaiter
    /// was ended as stopped, so await_suspend returns true and the frame stays
    /// suspended. False when the awaiter continues.
    bool end_stopped() noexcept {
        return stop_wanted() && stop_task(executor, nullptr, owner);
    }

    /// Continues the awaiter once its children are done: ends it as stopped, or
    /// resumes it.
    void continue_awaiter() noexcept {
        if (stop_wanted()) {
            // The entry names the awaiter's frame, which the stop can free
            // while the children still hold this state.
            if (park_) {
                executor->unpark(park_);
                park_ = nullptr;
            }
            stop_or_continue(executor, owner, awaiting_coroutine);
            return;
        }
        resume_continuation(executor, awaiting_coroutine);
    }

    ~CompletionState() {
        if (park_) executor->unpark(park_);
    }

    /// Called by await_suspend after deciding to suspend but before returning.
    void mark_suspended_and_check_completion() {
        // Children that are already done leave nothing to wait for, so the
        // registry is not touched.
        if (executor && !latch.settled()) {
            park_ = executor->park(this, &CompletionState::cancel_thunk);
        }
        const std::uint8_t before = latch.on_suspended();
        if (before &
            (CompletionLatch::BIT_COMPLETED | CompletionLatch::BIT_CANCELLED)) {
            if (!(before & CompletionLatch::BIT_COMPLETED)) {
                cancelled_ = true;
                stop_or_resume(executor, park_, awaiting_coroutine);
                return;
            }
            continue_awaiter();
        }
    }

    /// First step of await_resume: leaves the registry. Ends the process when
    /// the awaiter could not stop and shutdown, or a stopped child, resumed it.
    void finish_wait() {
        if (park_) {
            executor->unpark(park_);
            park_ = nullptr;
        }
        if (cancelled_) cancelled_unstoppable("a wait for tasks");
        if (stop_wanted()) cancelled_unstoppable("a wait for stopped tasks");
    }

    static void cancel_thunk(void* self) noexcept {
        static_cast<CompletionState*>(self)->cancel_parked();
    }

    void cancel_parked() noexcept {
        if (latch.on_cancelled()) {
            cancelled_ = true;
            stop_or_resume(executor, park_, awaiting_coroutine);
        }
    }

   private:
    ParkNode* park_ = nullptr;
    bool cancelled_ = false;
};

/// Hands the children of a vector when_all / when_any to its wrapper
/// coroutines. A wrapper takes the next free child, and the launcher starts one
/// more wrapper each time a wrapper suspends.
class ChildClaims {
   public:
    /// The index of the next child. At or past the child count when none is
    /// left. Relaxed: the read-modify-write makes each index unique, and the
    /// children were written before any wrapper started.
    std::size_t claim() noexcept {
        return next_.fetch_add(1, std::memory_order_relaxed);
    }

    /// True when every child is taken. A stale answer only starts a wrapper
    /// that finds nothing and ends.
    bool exhausted(std::size_t total) const noexcept {
        return next_.load(std::memory_order_relaxed) >= total;
    }

   private:
    std::atomic<std::size_t> next_{0};
};

/// All-of accounting: resumes only after every child completes, recording the
/// first exception observed.
struct WhenAllCompletionState : CompletionState {
    std::exception_ptr exception;
    std::atomic<bool> has_exception{false};
    std::atomic<std::size_t> completed_count{0};
    std::size_t total{0};

    void on_one_complete() {
        std::size_t count =
            completed_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == total) {
            if (latch.on_completed()) continue_awaiter();
        }
    }

    void on_exception(std::exception_ptr e) {
        bool expected = false;
        if (has_exception.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
            exception = e;
        }
        failed.store(true, std::memory_order_release);
        on_one_complete();
    }

    void on_stopped() {
        stopped.store(true, std::memory_order_release);
        on_one_complete();
    }
};

/// First-of accounting: the awaiter resumes once a winner is decided and every
/// child wrapper has finished, so no loser runs after it. One atomic holds both
/// facts, so the update that completes the condition releases the awaiter once.
struct WhenAnyCompletionState : CompletionState {
    std::size_t wrappers_total{0};

    void on_first_complete() {
        const std::size_t before =
            progress_.fetch_or(WINNER, std::memory_order_acq_rel);
        if ((before & FINISHED_MASK) == wrappers_total) release();
    }

    void on_wrapper_finished() {
        const std::size_t before =
            progress_.fetch_add(1, std::memory_order_acq_rel);
        if ((before & WINNER) != 0 &&
            (before & FINISHED_MASK) + 1 == wrappers_total) {
            release();
        }
    }

    bool released() const {
        const std::size_t now = progress_.load(std::memory_order_acquire);
        return (now & WINNER) != 0 && (now & FINISHED_MASK) == wrappers_total;
    }

   private:
    static constexpr std::size_t WINNER = std::size_t{1}
                                          << (sizeof(std::size_t) * 8 - 1);
    static constexpr std::size_t FINISHED_MASK = ~WINNER;

    void release() {
        if (latch.on_completed()) continue_awaiter();
    }

    std::atomic<std::size_t> progress_{0};
};

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_COMPLETION_STATE_H
