#include <dftracer/utils/core/io/awaitable.h>

namespace dftracer::utils::io {

void IoAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    handle_ = h;
    if (submit_ctx_ && submit_ctx_->submit) {
        submit_ctx_->submit(submit_ctx_, this);
    }
    // If submit_ctx_ is null, this is a bug -- should have been
    // caught by ready_ = true in the sync path.
}

}  // namespace dftracer::utils::io
