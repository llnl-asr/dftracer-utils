#include <dftracer/utils/common/error.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/run_loop.h>

#include <chrono>

namespace dftracer::utils {

void RunLoop::enqueue(std::coroutine_handle<> handle, TaskIndex /*task_id*/) {
    if (!handle) return;
    // Notify under the lock: once it drops, the owner can finish and destroy
    // this loop, cv included.
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.push_back(handle);
    ready_cv_.notify_one();
}

void RunLoop::schedule_coroutine_resumption(std::coroutine_handle<> handle) {
    enqueue(handle);
}

void RunLoop::schedule_destroy(std::coroutine_handle<> handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_destroy_.push_back(handle);
}

void RunLoop::mark_coro_completed(TaskIndex /*id*/) {}

io::IoBackend& RunLoop::io_backend() {
    throw DFTUtilsException(ErrorCode::Pipeline, "RunLoop has no I/O backend");
}

void RunLoop::drive_until(const std::function<bool()>& done) {
    while (!done()) {
        if (!run_one()) {
            if (done()) break;
            std::unique_lock<std::mutex> lock(mutex_);
            if (!ready_.empty()) continue;
            if (parked_.size() == 0) throw_stalled();
            // A waiter parked here is woken by enqueue() from another thread.
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
    drain_destroys();
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
        handle.resume();
    }
    // Released spawned frames (scope.spawn) route their destruction to the
    // thread-local queue via FinalAwaiter; the RunLoop is bound as the
    // current executor, so draining it here funnels them into pending_destroy_.
    drain_thread_local_destroys();
    drain_destroys();
    return true;
}

void RunLoop::drain_destroys() {
    // Frames are destroyed only once control has left them, never from
    // inside the resume that finished them.
    std::deque<std::coroutine_handle<>> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(pending_destroy_);
    }
    for (auto handle : batch) {
        if (handle) handle.destroy();
    }
}

void RunLoop::throw_stalled() {
    throw DFTUtilsException(
        ErrorCode::Pipeline,
        "RunLoop stalled: no runnable coroutines but the awaited work has "
        "not completed. Something is waiting on a resumption that will "
        "never arrive.");
}

}  // namespace dftracer::utils
