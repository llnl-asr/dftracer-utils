// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_TASK_ABI_H
#define DFTRACER_UTILS_CORO_TASK_ABI_H

#include <dftracer/utils/common/abi.h>
#include <dftracer/utils/coro/abi.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::coro {

/// The host side of every dftu_task: its body and how it ended. Heap-owned;
/// whoever awaits it frees it with @ref free_abi_task.
struct AbiTask {
    dstd::Optional<CoroTask<void>> body;
    dstd::String message;
    dftu_status status{DFTU_OK};
    bool awaited{false};
    bool finished{false};
    /// The body ended as cancelled rather than with a status.
    bool stopped{false};

    void fail(dftu_status s, dstd::StringView text) {
        status = s;
        message.assign(text.data(), text.size());
    }
};

/// A task whose body make(&task) builds; the body reports failure through
/// AbiTask::fail.
template <class Make>
dftu_task* make_abi_task(Make&& make) {
    auto task = dstd::make_unique<AbiTask>();
    task->body.emplace(make(task.get()));
    return reinterpret_cast<dftu_task*>(task.release());
}

/// A task over a body that cannot fail.
dftu_task* task_to_abi(CoroTask<void>&& body);

inline AbiTask* abi_task(dftu_task* task) noexcept {
    return reinterpret_cast<AbiTask*>(task);
}

/// Awaits the body once and gives the status; later calls give the same
/// status without waiting. A stop gives DFTU_E_CANCELLED and sets `stopped`.
CoroTask<dftu_status> run_abi_task(AbiTask* task);

/// The error of a finished task with a negative status.
dstd::Error abi_task_error(const AbiTask& task);

void free_abi_task(dftu_task* task) noexcept;

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_TASK_ABI_H
