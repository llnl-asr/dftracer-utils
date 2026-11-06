// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/hooks.h>
#include <dftracer/utils/runtime/io/awaitable.h>

namespace dftracer::utils::runtime::io {

namespace {

void resume_waiter(IoOp& op) noexcept {
    auto& self = static_cast<IoAwaitable&>(op);
    self.resume_on->post(self.waiter);
}

}  // namespace

void IoAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    waiter = h;
    if (coro::Executor* current = coro::Executor::current()) {
        resume_on = current;
    }
    complete = &resume_waiter;
    // Coroutines reached by symmetric transfer never pass through the executor
    // queue until they suspend here; register now (parent = the resuming
    // ancestor) so the I/O completion's re-enqueue does not orphan them.
    if (const coro::CoroutineHooks* hooks = coro::coroutine_hooks()) {
        hooks->enqueue(hooks->user, h.address(), coro::CoroutineOrigin::Io);
    }
    backend->submit(*this);
}

}  // namespace dftracer::utils::runtime::io
