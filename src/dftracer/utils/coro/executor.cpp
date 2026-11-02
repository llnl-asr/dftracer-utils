// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/resumption_helper.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/vector.h>

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdlib>

namespace dftracer::utils::coro {

static thread_local Executor* tls_current_executor = nullptr;

Executor* Executor::current() noexcept { return tls_current_executor; }

Executor* Executor::set_current(Executor* e) noexcept {
    auto* old = tls_current_executor;
    tls_current_executor = e;
    return old;
}

namespace {

// Coroutine handles to destroy after the current resume() returns, so another
// worker never frees a frame while its suspend is still running. The inline
// slots keep the common drain free of allocation, and the overflow is freed
// after each drain, so no counted block outlives it.
struct PendingDestroys {
    static constexpr std::size_t INLINE = 16;
    std::array<std::coroutine_handle<>, INLINE> handles{};
    std::size_t count = 0;
    dstd::Vector<std::coroutine_handle<>> overflow;
};

thread_local PendingDestroys tls_pending_destroys;

}  // namespace

void schedule_thread_local_destroy(std::coroutine_handle<> h) {
    PendingDestroys& p = tls_pending_destroys;
    if (p.count < PendingDestroys::INLINE) {
        p.handles[p.count++] = h;
    } else {
        p.overflow.push_back(h);
    }
}

void drain_thread_local_destroys() {
    PendingDestroys& p = tls_pending_destroys;
    // A destroy can schedule more, so both loops read the size each round.
    for (std::size_t i = 0; i < p.count; ++i) {
        if (auto h = p.handles[i]) h.destroy();
    }
    for (std::size_t i = 0; i < p.overflow.size(); ++i) {
        if (auto h = p.overflow[i]) h.destroy();
    }
    p.count = 0;
    if (!p.overflow.empty())
        dstd::Vector<std::coroutine_handle<>>().swap(p.overflow);
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
