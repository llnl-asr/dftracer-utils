#ifndef DFTRACER_UTILS_CORE_CORO_COMPLETION_LATCH_H
#define DFTRACER_UTILS_CORE_CORO_COMPLETION_LATCH_H

#include <dftracer/utils/core/coro/resumption_helper.h>

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace dftracer::utils {
class Executor;
namespace coro {

// Coordinates an awaiter's suspension with child completion so exactly one side
// resumes the awaiting coroutine. Both sides fetch_or their bit on one atomic;
// whichever observes the other's bit already set is the side that resumes
// (single modification order => no lost or double wakeup).
class CompletionLatch {
   public:
    // The awaiter has suspended. Returns true if completion already happened
    // (the caller must then resume the continuation).
    bool on_suspended() noexcept {
        return (state_.fetch_or(BIT_SUSPENDED, std::memory_order_acq_rel) &
                BIT_COMPLETED) != 0;
    }

    // Completion finished. Returns true if the awaiter already suspended (the
    // caller must then resume the continuation).
    bool on_completed() noexcept {
        return (state_.fetch_or(BIT_COMPLETED, std::memory_order_acq_rel) &
                BIT_SUSPENDED) != 0;
    }

   private:
    static constexpr std::uint8_t BIT_SUSPENDED = 1;
    static constexpr std::uint8_t BIT_COMPLETED = 2;
    std::atomic<std::uint8_t> state_{0};
};

// Resume `continuation` via the executor if present, else inline, guarding a
// null/already-done handle. Used after the latch says this side won the race.
inline void resume_continuation(Executor* executor,
                                std::coroutine_handle<> continuation) {
    if (continuation && !continuation.done()) {
        if (executor) {
            schedule_coroutine_resumption_helper(executor, continuation);
        } else {
            continuation.resume();
        }
    }
}

}  // namespace coro
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_CORO_COMPLETION_LATCH_H
