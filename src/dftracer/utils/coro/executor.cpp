// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/resumption_helper.h>
#include <dftracer/utils/coro/task.h>

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace dftracer::utils::coro {

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

void post_resumption(Executor* executor, std::coroutine_handle<> handle) {
    if (auto* target = resume_executor_for(executor)) {
        target->post(handle);
        return;
    }
    // Every task carries an executor, so reaching here is a bug. Dropping
    // the resume would strand the coroutine as a hang far from the cause.
    DFTRACER_UTILS_LOG_ERROR(
        "%s", "Coroutine resumption has no executor to resume through");
    std::abort();
}

namespace detail {

bool runs_on(Executor* home) noexcept {
    // A thread that runs no executor resumes the awaiter itself, as before
    // affinity. Posting from it could reach an executor that already stopped.
    Executor* here = Executor::current();
    return home == nullptr || here == nullptr || home == here;
}

void resume_on_home(Executor* home, std::coroutine_handle<> awaiter) noexcept {
    home->post(awaiter);
}

Executor* current_executor() noexcept { return Executor::current(); }

}  // namespace detail

}  // namespace dftracer::utils::coro

namespace dftracer::utils {

std::size_t common::available_parallelism() noexcept {
    auto* exec = coro::Executor::current();
    return exec ? exec->concurrency() : 1;
}

}  // namespace dftracer::utils
