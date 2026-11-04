// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/intern_table.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>

namespace dftracer::utils::coro::detail {

RunContext::RunContext() noexcept {
    if (auto* table = intern()) table->add_ref();
}

RunContext::~RunContext() {
    if (auto* table = intern()) table->release();
}

std::coroutine_handle<> CoroPromise::FinalAwaiter::await_suspend(
    std::coroutine_handle<CoroPromise> h) noexcept {
    auto& p = h.promise();

    // Snapshot ALL promise fields we need BEFORE any atomic
    // decrement.  After fetch_sub, the joiner may run on another
    // thread, destroying the JoinHandle (and its continuation_
    // field).  Reading through p after fetch_sub is UB if another
    // thread freed the containing scope.
    const bool was_released = p.released;
    auto* const jc = p.join_counter;
    auto* const jcont = p.join_continuation;
    TaskTracking* const tracker = p.tracking;

    // Report a tracked task as completed whatever the join-group path does.
    if (tracker != nullptr) {
        p.tracking = nullptr;
        tracker->on_complete(tracker->owner, tracker->id);
    }

    // Handle join group (JoinHandle integration).
    if (jc) {
        auto prev = jc->fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1 && jcont) {
            auto cont_addr =
                jcont->exchange(nullptr, std::memory_order_acq_rel);
            if (cont_addr) {
                if (was_released) {
                    schedule_thread_local_destroy(std::coroutine_handle<>(h));
                }
                return std::coroutine_handle<>::from_address(cont_addr);
            }
        }
        if (was_released) {
            schedule_thread_local_destroy(std::coroutine_handle<>(h));
        }
        return std::noop_coroutine();
    }

    if (was_released) {
        schedule_thread_local_destroy(std::coroutine_handle<>(h));
    }
    return std::noop_coroutine();
}

}  // namespace dftracer::utils::coro::detail
