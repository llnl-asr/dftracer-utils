// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/monitor.h>
#include <dftracer/utils/runtime/io/awaitable.h>

namespace dftracer::utils::io {

void IoAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    handle_ = h;
    // Coroutines reached by symmetric transfer never pass through the executor
    // queue until they suspend here; register now (parent = the resuming
    // ancestor) so the I/O completion's re-enqueue does not orphan them.
    if (coro::monitoring_enabled()) {
        coro::monitor_enqueue(h.address(), coro::CoroKind::Io);
    }
    if (submit_ctx_ && submit_ctx_->submit) {
        submit_ctx_->submit(submit_ctx_, this);
    }
    // If submit_ctx_ is null, this is a bug -- should have been
    // caught by ready_ = true in the sync path.
}

}  // namespace dftracer::utils::io
