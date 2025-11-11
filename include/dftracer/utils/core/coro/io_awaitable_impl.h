#ifndef DFTRACER_UTILS_CORE_CORO_IO_AWAITABLE_IMPL_H
#define DFTRACER_UTILS_CORE_CORO_IO_AWAITABLE_IMPL_H

#include <dftracer/utils/core/coro/io_awaitable.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/io_executor.h>

namespace dftracer::utils::coro {

template <typename T>
template <typename Promise>
bool IOAwaitable<T>::await_suspend(std::coroutine_handle<Promise> h) {
    continuation_ = h;

    // Mark as awaiting async work
    if constexpr (requires { h.promise().awaiting_async_; }) {
        h.promise().awaiting_async_ = true;
    }

    // Try to get IOExecutor from the promise chain
    IOExecutor* io_executor = nullptr;
    if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
        auto* root = h.promise().get_root_promise();
        if (root) {
            Executor* executor = root->get_executor();
            if (executor) {
                io_executor = executor->get_io_executor();
            }
        }
    }

    // If IOExecutor is available and running, submit async I/O
    if (io_executor && io_executor->is_running() && io_func_) {
        // Create a wrapper that captures this IOAwaitable's state
        auto* self = this;
        io_executor->submit_io_operation(
            [self]() {
                // Execute I/O and store result
                try {
                    self->result_ = self->io_func_();
                } catch (...) {
                    self->exception_ = std::current_exception();
                }
                self->ready_ = true;
            },
            h);
        // Suspend - IOExecutor will resume when complete
        return true;
    }

    // Fallback: Execute synchronously if no IOExecutor
    try {
        if (io_func_) {
            result_ = io_func_();
        }
        ready_ = true;
    } catch (...) {
        exception_ = std::current_exception();
        ready_ = true;
    }

    // Clear async flag since we executed synchronously
    if constexpr (requires { h.promise().awaiting_async_; }) {
        h.promise().awaiting_async_ = false;
    }

    // Don't suspend - result is ready
    return false;
}

// Specialization for void
template <typename Promise>
bool IOAwaitable<void>::await_suspend(std::coroutine_handle<Promise> h) {
    continuation_ = h;

    // Mark as awaiting async work
    if constexpr (requires { h.promise().awaiting_async_; }) {
        h.promise().awaiting_async_ = true;
    }

    // Try to get IOExecutor from the promise chain
    IOExecutor* io_executor = nullptr;
    if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
        auto* root = h.promise().get_root_promise();
        if (root) {
            Executor* executor = root->get_executor();
            if (executor) {
                io_executor = executor->get_io_executor();
            }
        }
    }

    // If IOExecutor is available and running, submit async I/O
    if (io_executor && io_executor->is_running() && io_func_) {
        // Create a wrapper that captures this IOAwaitable's state
        auto* self = this;
        io_executor->submit_io_operation(
            [self]() {
                // Execute I/O
                try {
                    self->io_func_();
                } catch (...) {
                    self->exception_ = std::current_exception();
                }
                self->ready_ = true;
            },
            h);
        // Suspend - IOExecutor will resume when complete
        return true;
    }

    // Fallback: Execute synchronously if no IOExecutor
    try {
        if (io_func_) {
            io_func_();
        }
        ready_ = true;
    } catch (...) {
        exception_ = std::current_exception();
        ready_ = true;
    }

    // Clear async flag since we executed synchronously
    if constexpr (requires { h.promise().awaiting_async_; }) {
        h.promise().awaiting_async_ = false;
    }

    // Don't suspend - result is ready
    return false;
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_IO_AWAITABLE_IMPL_H
