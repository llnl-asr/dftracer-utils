// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/run_loop.h>
#include <dftracer/utils/memory/manager.h>

#include <chrono>

namespace dftracer::utils::coro {

void RunLoop::post(std::coroutine_handle<> handle) noexcept {
    if (!handle) return;
    // Notify under the lock: once it drops, the owner can finish and destroy
    // this loop, cv included.
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.push_back(handle);
    ready_cv_.notify_one();
}

bool RunLoop::drive_until(dstd::FunctionRef<bool()> done) {
    while (!done()) {
        if (!run_one()) {
            if (done()) break;
            std::unique_lock<std::mutex> lock(mutex_);
            if (!ready_.empty()) continue;
            if (parked_.size() == 0) throw_stalled();
            // A waiter parked here is woken by post() from another thread.
            ready_cv_.wait_for(lock, std::chrono::milliseconds(10),
                               [&] { return !ready_.empty() || done(); });
        }
    }
    // Work the finished task left runnable (a producer woken to unwind after
    // its consumer stopped early) has only this loop to run on; dropping it
    // here would leak every frame it still holds.
    while (run_one()) {
    }
    drain_thread_local_destroys();
    return true;
}

bool RunLoop::run_one() {
    std::coroutine_handle<> handle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_.empty()) return false;
        handle = ready_.front();
        ready_.pop_front();
    }
    if (handle && !handle.done()) {
        memory::Scope scope{memory::Manager::get().general(), nullptr};
        // Resumed under this loop as the current executor, so frames released
        // during the resume route their destruction to the thread-local queue
        // drained below.
        resume_on(*this, handle);
    }
    // Released spawned frames (scope.spawn) route their destruction to the
    // thread-local queue via FinalAwaiter.
    drain_thread_local_destroys();
    return true;
}

void RunLoop::throw_stalled() {
    throw common::DFTUtilsException(
        common::ErrorCode::Pipeline,
        "RunLoop stalled: no runnable coroutines but the awaited work has "
        "not completed. Something is waiting on a resumption that will "
        "never arrive.");
}

}  // namespace dftracer::utils::coro
