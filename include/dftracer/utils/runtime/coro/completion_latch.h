#ifndef DFTRACER_UTILS_RUNTIME_CORO_COMPLETION_LATCH_H
#define DFTRACER_UTILS_RUNTIME_CORO_COMPLETION_LATCH_H

#include <dftracer/utils/runtime/executor/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace dftracer::utils {
class Executor;
namespace coro {

/// Coordinates an awaiter's suspension with child completion so exactly one
/// side resumes the awaiting coroutine. Each side fetch_ors its bit on one
/// atomic; whichever observes the right bits already set is the side that
/// resumes (single modification order => no lost or double wakeup). A third
/// party, executor shutdown, may cancel the wait: it resumes the awaiter only
/// when completion has not.
class CompletionLatch {
   public:
    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    static constexpr std::uint8_t BIT_CANCELLED = 4;

    /// True when completion or cancellation already happened.
    bool settled() const noexcept {
        return (state_.load(std::memory_order_acquire) &
                (BIT_COMPLETED | BIT_CANCELLED)) != 0;
    }

    /// The awaiter has suspended. Returns the bits that were set before: the
    /// caller must resume the continuation when COMPLETED or CANCELLED was
    /// already there.
    std::uint8_t on_suspended() noexcept {
        return state_.fetch_or(BIT_SUSPENDED, std::memory_order_acq_rel);
    }

    /// Completion finished. Returns true if the awaiter already suspended and
    /// no cancellation resumed it (the caller must then resume the
    /// continuation).
    bool on_completed() noexcept {
        const std::uint8_t prev =
            state_.fetch_or(BIT_COMPLETED, std::memory_order_acq_rel);
        return (prev & BIT_SUSPENDED) != 0 && (prev & BIT_CANCELLED) == 0;
    }

    /// The wait was cancelled. Returns true if the awaiter already suspended
    /// and nothing resumed it yet (the caller must then resume the
    /// continuation, which is to report the cancellation).
    bool on_cancelled() noexcept {
        const std::uint8_t prev =
            state_.fetch_or(BIT_CANCELLED, std::memory_order_acq_rel);
        return (prev & BIT_SUSPENDED) != 0 &&
               (prev & (BIT_COMPLETED | BIT_CANCELLED)) == 0;
    }

   private:
    std::atomic<std::uint8_t> state_{0};
};

/// Resume `continuation` after the latch says this side won the race.
/// Always through its executor: resuming inline would run the coroutine on
/// whichever thread happened to win, which is not the one it was parked on.
inline void resume_continuation(Executor* executor,
                                std::coroutine_handle<> continuation) {
    if (continuation && !continuation.done()) {
        schedule_coroutine_resumption_helper(executor, continuation);
    }
}

}  // namespace coro
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_CORO_COMPLETION_LATCH_H
