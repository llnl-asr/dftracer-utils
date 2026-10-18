#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/core/executor/executor.h>
#include <dftracer/utils/core/executor/resumption_helper.h>

#include <coroutine>
#include <cstddef>
#include <vector>

namespace dftracer::utils {

static thread_local Executor* tls_current_executor = nullptr;

Executor* Executor::current() noexcept { return tls_current_executor; }

Executor* Executor::set_current(Executor* e) noexcept {
    auto* old = tls_current_executor;
    tls_current_executor = e;
    return old;
}

// Thread-local list of coroutine handles to destroy after the current
// resume() returns.  FinalAwaiter pushes here instead of the shared
// destroy_queue_ to avoid another worker freeing the frame while
// the coroutine-suspend machinery is still accessing it.
static thread_local std::vector<std::coroutine_handle<>> tls_pending_destroys;

void schedule_thread_local_destroy(std::coroutine_handle<> h) {
    tls_pending_destroys.push_back(h);
}

void drain_thread_local_destroys() {
    for (std::size_t i = 0; i < tls_pending_destroys.size(); ++i) {
        if (auto h = tls_pending_destroys[i]) h.destroy();
    }
    tls_pending_destroys.clear();
}

Executor* resume_executor_for(Executor* preferred) noexcept {
    return preferred ? preferred : Executor::current();
}

void schedule_coroutine_resumption_helper(Executor* executor,
                                          std::coroutine_handle<> handle) {
    if (auto* target = resume_executor_for(executor)) {
        target->schedule_coroutine_resumption(handle);
        return;
    }
    // Returning quietly here strands the coroutine forever, which surfaces
    // far from the cause as a hang.
    DFTRACER_UTILS_LOG_ERROR(
        "%s", "Coroutine resumption dropped: no executor to resume through");
}

// Helper function for when_any.h (avoids circular dependency)
void schedule_destroy_helper(Executor* executor,
                             std::coroutine_handle<> handle) {
    if (executor && handle) {
        executor->schedule_destroy(handle);
    }
}

std::size_t available_parallelism() noexcept {
    auto* exec = Executor::current();
    return exec ? exec->get_num_threads() : 1;
}

}  // namespace dftracer::utils
