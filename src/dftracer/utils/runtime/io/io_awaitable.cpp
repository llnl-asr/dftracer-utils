// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/hooks.h>
#include <dftracer/utils/runtime/io/awaitable.h>

namespace dftracer::utils::runtime::io {

void IoAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    handle = h;
    resume_on = coro::Executor::current();
    // Coroutines reached by symmetric transfer never pass through the executor
    // queue until they suspend here; register now (parent = the resuming
    // ancestor) so the I/O completion's re-enqueue does not orphan them.
    if (const coro::CoroutineHooks* hooks = coro::coroutine_hooks()) {
        hooks->enqueue(hooks->user, h.address(), coro::CoroutineOrigin::Io);
    }
    if (submit_ctx && submit_ctx->submit) {
        submit_ctx->submit(submit_ctx, this);
    }
    // If submit_ctx is null, this is a bug -- should have been
    // caught by is_ready = true in the sync path.
}

void IoAwaitable::resume_through(coro::Executor& fallback) noexcept {
    coro::Executor* target = resume_on != nullptr ? resume_on : &fallback;
    target->post(handle);
}

}  // namespace dftracer::utils::runtime::io
