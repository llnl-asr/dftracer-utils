// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_TASK_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_TASK_H

#include <dftracer/utils/common/abi.h>
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/coro/abi_context.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::plugins::host {

/// The error of a plugin callback @p what that returned the negative
/// @p status, carrying the message it set on @p ctx.
inline dstd::Error plugin_error(const coro::AbiContext& ctx, dftu_status status,
                                dstd::StringView what) {
    dstd::StringView message = ctx.message();
    if (message.empty()) message = "failed";
    return dstd::Error::from_format<"{}: {}">(common::to_condition(status),
                                              what, message);
}

/// Owns a handle a plugin call wrote and frees it when the holder dies, which
/// covers a coroutine frame destroyed by a stop.
template <class T, void (*Free)(T*)>
class AbiHandle {
   public:
    AbiHandle() = default;
    explicit AbiHandle(T* p) : p_(p) {}
    AbiHandle(const AbiHandle&) = delete;
    AbiHandle& operator=(const AbiHandle&) = delete;
    ~AbiHandle() {
        if (p_) Free(p_);
    }

    T** slot() { return &p_; }
    T* get() const { return p_; }
    T* release() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }

   private:
    T* p_ = nullptr;
};

/// Awaits the task a plugin returned and frees it, giving its status (DFTU_OK
/// or DFTU_END). A stop of the caller ends the caller as stopped; a failed task
/// gives its error, and a task that throws is a failed task.
inline coro::CoroTask<dstd::Result<dftu_status>> await_plugin_task(
    ::dftu_task* t) {
    coro::AbiTask* task = coro::abi_task(t);
    const dftu_status status = co_await coro::run_abi_task(task);
    if (task->stopped) {
        coro::free_abi_task(t);
        co_await coro::stop_here();
    }
    if (status < 0) {
        dstd::Error failure = coro::abi_task_error(*task);
        coro::free_abi_task(t);
        co_return failure;
    }
    coro::free_abi_task(t);
    co_return status;
}

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_TASK_H
