#ifndef DFTRACER_UTILS_CORE_IO_AWAITABLE_H
#define DFTRACER_UTILS_CORE_IO_AWAITABLE_H

#include <fcntl.h>
#include <sys/types.h>

#include <coroutine>
#include <cstddef>

namespace dftracer::utils::io {

struct IoAwaitable;

/// Submission context -- base struct for backend-specific data.
/// The first member is always a function pointer that await_suspend
/// calls to trigger the actual I/O submission. Backends derive from
/// this (or embed it) to add operation-specific fields.
struct SubmitContext {
    using SubmitFn = void (*)(SubmitContext*, IoAwaitable*);
    SubmitFn submit = nullptr;
};

/// Result of an async I/O operation.
/// Holds the return value of the underlying syscall (bytes read/written,
/// fd for open, 0 for close) or a negative errno on failure.
struct IoAwaitable {
    // Set by the backend when the operation completes
    ssize_t result_ = 0;

    // Coroutine handle to resume on completion
    std::coroutine_handle<> handle_{};

    // Opaque pointer to a SubmitContext (backend-specific).
    // The backend creates a heap-allocated SubmitContext subclass in
    // submit_*(), stores a pointer here. await_suspend calls
    // ctx->submit(ctx, this) to trigger the I/O operation.
    // The backend is responsible for freeing the SubmitContext.
    SubmitContext* submit_ctx_ = nullptr;

    // True when result is already available (sync fallback path)
    bool ready_ = false;

    bool await_ready() const noexcept { return ready_; }

    // Declared here, defined in io_awaitable.cpp
    void await_suspend(std::coroutine_handle<> h) noexcept;

    ssize_t await_resume() const noexcept { return result_; }

    /// Create an already-completed awaitable (for sync fallback)
    static IoAwaitable ready(ssize_t result) noexcept {
        IoAwaitable a;
        a.result_ = result;
        a.ready_ = true;
        return a;
    }
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_AWAITABLE_H
