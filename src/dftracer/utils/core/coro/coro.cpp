#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/pipeline/executor.h>

namespace dftracer::utils::coro {

std::coroutine_handle<> CoroPromise::FinalAwaiter::await_suspend(
    std::coroutine_handle<CoroPromise> h) noexcept {
    auto& p = h.promise();

    // Snapshot ALL promise fields we need BEFORE any atomic
    // decrement.  After fetch_sub, the joiner may run on another
    // thread, destroying the JoinHandle (and its continuation_
    // field).  Reading through p after fetch_sub is UB if another
    // thread freed the containing scope.
    const bool was_released = p.released;
    auto* const join_counter = p.join_counter;
    auto* const join_cont = p.join_continuation;

    // Handle join group (JoinHandle integration).
    if (join_counter) {
        auto prev = join_counter->fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1 && join_cont) {
            // Last coro in join group -- atomically take the
            // continuation.  exchange(nullptr) ensures exactly one
            // side gets the handle (no double-resume).
            //
            // We use the snapshotted join_cont pointer here.  This
            // is safe: we are the LAST decrement (prev==1), so the
            // joiner has not been resumed yet and the JoinHandle
            // memory is still alive.
            auto cont_addr =
                join_cont->exchange(nullptr, std::memory_order_acq_rel);
            if (cont_addr) {
                // Defer destruction to AFTER resume() returns on
                // this worker thread.
                if (was_released) {
                    dftracer::utils::schedule_thread_local_destroy(
                        std::coroutine_handle<>(h));
                }
                return std::coroutine_handle<>::from_address(cont_addr);
            }
        }
        // Non-last coro, or continuation was null.
        if (was_released) {
            dftracer::utils::schedule_thread_local_destroy(
                std::coroutine_handle<>(h));
        }
        return std::noop_coroutine();
    }

    // Not part of a join group (abnormal path).
    if (was_released) {
        dftracer::utils::schedule_thread_local_destroy(
            std::coroutine_handle<>(h));
    }
    return std::noop_coroutine();
}

}  // namespace dftracer::utils::coro
