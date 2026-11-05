// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_AWAITABLE_H
#define DFTRACER_UTILS_RUNTIME_IO_AWAITABLE_H

#include <fcntl.h>
#include <sys/types.h>

#include <coroutine>
#include <cstddef>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::runtime::io {

struct IoAwaitable;

/// A blocking call for @ref call to run. It must not throw. A failure returns
/// a negative result with errno set, as a system call does.
using BlockingFn = ssize_t (*)(void* context) noexcept;

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
    /// Set by the backend when the operation completes
    ssize_t result = 0;

    /// Coroutine handle to resume on completion
    std::coroutine_handle<> handle{};

    /// The executor of the task that awaits this operation, recorded when it
    /// suspends. The completion resumes the task there (task affinity).
    coro::Executor* resume_on = nullptr;

    /// Opaque pointer to a @ref SubmitContext (backend-specific).
    /// The backend creates a heap-allocated @ref SubmitContext subclass in
    /// submit_*(), stores a pointer here. await_suspend calls
    /// ctx->submit(ctx, this) to trigger the I/O operation.
    /// The backend is responsible for freeing the @ref SubmitContext.
    SubmitContext* submit_ctx = nullptr;

    /// True when result is already available (sync fallback path)
    bool is_ready = false;

    bool await_ready() const noexcept { return is_ready; }

    /// Declared here, defined in io_awaitable.cpp
    void await_suspend(std::coroutine_handle<> h) noexcept;

    ssize_t await_resume() const noexcept { return result; }

    /// Posts the awaiting task to its own executor, or to `fallback` (the
    /// backend's executor) when it suspended outside any executor.
    void resume_through(coro::Executor& fallback) noexcept;

    /// Create an already-completed awaitable (for sync fallback)
    static IoAwaitable ready(ssize_t result) noexcept {
        IoAwaitable a;
        a.result = result;
        a.is_ready = true;
        return a;
    }
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_AWAITABLE_H
