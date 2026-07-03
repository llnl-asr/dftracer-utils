#ifndef DFTRACER_UTILS_CORE_CORO_COMPLETION_STATE_H
#define DFTRACER_UTILS_CORE_CORO_COMPLETION_STATE_H

#include <dftracer/utils/core/coro/completion_latch.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>

namespace dftracer::utils {
class Executor;
namespace coro {

// Common suspension/completion coordination shared by when_all / when_any
// shared-state structs. Holds the awaiting coroutine, its executor, and the
// CompletionLatch that guards the suspend-vs-complete race.
struct CompletionState {
    std::coroutine_handle<> awaiting_coroutine_{};
    Executor* executor_{nullptr};
    CompletionLatch latch_;

    // Called by await_suspend after deciding to suspend but before returning.
    void mark_suspended_and_check_completion() {
        if (latch_.on_suspended())
            resume_continuation(executor_, awaiting_coroutine_);
    }
};

// All-of accounting: resumes only after every child completes, recording the
// first exception observed.
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

// First-of accounting: resumes as soon as the first child completes (winner of
// the latch).
struct WhenAnyCompletionState : CompletionState {
    void on_first_complete() {
        if (latch_.on_completed())
            resume_continuation(executor_, awaiting_coroutine_);
    }
};

}  // namespace coro
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_CORO_COMPLETION_STATE_H
