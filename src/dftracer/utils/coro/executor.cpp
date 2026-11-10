// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/detail/channel_queue.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/vector.h>

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdlib>

namespace dftracer::utils::coro {

static thread_local Executor* t_current_executor = nullptr;

Executor* Executor::current() noexcept { return t_current_executor; }

Executor* Executor::set_current(Executor* e) noexcept {
    auto* old = t_current_executor;
    t_current_executor = e;
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

thread_local PendingDestroys t_pending_destroys;

}  // namespace

void schedule_thread_local_destroy(std::coroutine_handle<> h) {
    PendingDestroys& p = t_pending_destroys;
    if (p.count < PendingDestroys::INLINE) {
        p.handles[p.count++] = h;
    } else {
        p.overflow.push_back(h);
    }
}

void drain_thread_local_destroys() {
    PendingDestroys& p = t_pending_destroys;
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

namespace detail {

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
        "{}", "Coroutine resumption has no executor to resume through");
    std::abort();
}

namespace {
struct ParkThreadState {
    ParkOwner owner;
    bool parked_after_cancel{false};
};
thread_local ParkThreadState t_park;
}  // namespace

namespace {
// A frame that runs once and frees itself, so a cancel callback can hand the
// stop of a waiter to the executor and not run it under the registry lock.
struct StopJob {
    struct promise_type {
        StopJob get_return_object() noexcept {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            dstd::fatal("coro: a stop job failed");
        }
    };
    std::coroutine_handle<promise_type> handle;
};

StopJob run_stop(Executor* executor, ParkNode* node, ParkOwner owner) {
    if (node != nullptr) executor->unpark(node);
    if (!owner.ops->stop(owner.promise)) {
        cancelled_unstoppable("a wait in a task that holds a scope");
    }
    co_return;
}
}  // namespace

bool stop_task(Executor* executor, ParkNode* node, ParkOwner owner) noexcept {
    if (owner.ops == nullptr || owner.ops->stop == nullptr ||
        owner.ops->refuses_stop(owner.promise)) {
        return false;
    }
    if (executor == nullptr) return owner.ops->stop(owner.promise);
    StopJob job = run_stop(executor, node, owner);
    executor->post(job.handle);
    return true;
}

bool stop_parked(Executor* executor, ParkNode* node) noexcept {
    if (node == nullptr) return false;
    return stop_task(executor, node, node->owner);
}

void stop_or_continue(Executor* executor, ParkOwner owner,
                      std::coroutine_handle<> handle) noexcept {
    if (stop_task(executor, nullptr, owner)) return;
    post_resumption(executor, handle);
}

void stop_or_resume(Executor* executor, ParkNode*& park,
                    std::coroutine_handle<> handle) noexcept {
    ParkNode* node = park;
    park = nullptr;
    if (stop_parked(executor, node)) return;
    park = node;
    post_resumption(executor, handle);
}

void cancelled_unstoppable(const char* what) noexcept {
    dstd::fatal("coro: a cancel reached a wait that cannot stop", what);
}

void wake_cancelled_on(Executor* executor) noexcept {
    executor->wake_cancelled();
}

ParkOwner current_park_owner() noexcept { return t_park.owner; }

void note_park_after_cancel() noexcept { t_park.parked_after_cancel = true; }

ParkOwnerScope::ParkOwnerScope(ParkOwner owner) noexcept
    : saved_(t_park.owner) {
    t_park.owner = owner;
}

ParkOwnerScope::~ParkOwnerScope() {
    t_park.owner = saved_;
    if (!t_park.parked_after_cancel) return;
    t_park.parked_after_cancel = false;
    if (Executor* executor = Executor::current()) executor->wake_cancelled();
}

void abort_zero_channel_capacity() {
    DFTRACER_UTILS_LOG_ERROR(
        "{}", "Channel capacity must be at least 1; use UnboundedChannel");
    std::abort();
}

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
