#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/run_loop.h>

#include <utility>

namespace dftracer::utils {

void RunLoop::enqueue(std::coroutine_handle<> handle, TaskIndex /*task_id*/) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.push_back(handle);
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
    throw DFTUtilsException(ErrorCode::PIPELINE, "RunLoop has no I/O backend");
}

void RunLoop::drive_until(const std::function<bool()>& done) {
    while (!done()) {
        if (!run_one()) {
            if (done()) break;
            throw_stalled();
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
    if (handle && !handle.done()) handle.resume();
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
        ErrorCode::PIPELINE,
        "RunLoop stalled: no runnable coroutines but the awaited work has "
        "not completed. Something is waiting on a resumption that will "
        "never arrive.");
}

}  // namespace dftracer::utils
