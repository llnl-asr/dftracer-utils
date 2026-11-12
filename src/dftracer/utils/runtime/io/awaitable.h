// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_AWAITABLE_H
#define DFTRACER_UTILS_RUNTIME_IO_AWAITABLE_H

#include <dftracer/utils/runtime/io/backend.h>

#include <coroutine>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::runtime::io {

/// An @ref IoOp that a coroutine awaits. It lives in the coroutine frame.
/// Awaiting it gives the value of the system call or a negative errno. With no
/// backend the call has already run, on the calling thread.
struct IoAwaitable : IoOp {
    std::coroutine_handle<> waiter{};

    /// The executor that resumes the task.
    coro::Executor* resume_on = nullptr;

    IoBackend* backend = nullptr;

    bool await_ready() const noexcept { return backend == nullptr; }

    void await_suspend(std::coroutine_handle<> h) noexcept;

    ssize_t await_resume() const noexcept { return result; }
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_AWAITABLE_H
