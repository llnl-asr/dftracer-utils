// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_TASK_ABI_H
#define DFTRACER_UTILS_PLUGINS_TASK_ABI_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/core.h>

#include <atomic>
#include <cstddef>

namespace dftracer::utils::plugins {

struct AbiTask;

/// What a task needs to end without a wait: `payload` is for `now` and
/// `later` to share.
struct AbiFast {
    dstd::Vector<std::byte> payload;
    /// Ends the task at once and gives true when that needs no wait. Then the
    /// task makes no body and no scope.
    dstd::Function<bool(AbiTask*)> now;
    /// Makes the body when `now` gave false.
    dstd::Function<coro::CoroTask<void>(AbiTask*)> later;
};

/// The host side of every dftu_task: its body and how it ended. Heap-owned;
/// whoever awaits it frees it with @ref free_abi_task.
struct AbiTask {
    dstd::Optional<coro::CoroTask<void>> body;
    /// Set on a task that may end without a wait; null on the rest.
    dstd::UniquePtr<AbiFast> fast;
    dstd::String message;
    dftu_status status{DFTU_OK};
    bool awaited{false};
    /// Set after `status` and `message`, so a reader that sees it reads them.
    std::atomic<bool> finished{false};
    /// The body ended as cancelled by its awaiter rather than with a status.
    bool stopped{false};
    /// Set by @ref cancel_abi_task. The body's promise reads it as its cancel
    /// source, so the body, and what it awaits, end as cancelled at their
    /// next wait, with no stop source or scope made for a task never cancelled.
    std::atomic<bool> cancel_requested{false};
    /// The node of the cancel chain for `cancel_requested`.
    coro::CancelLink link{&cancel_requested};
    /// The references to the task: the owner's, and one for each thread that
    /// called @ref retain_abi_task. The last @ref free_abi_task deletes it.
    std::atomic<int> refs{1};
    /// The executor that awaits the task, set once it is awaited.
    std::atomic<coro::Executor*> executor{nullptr};

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

/// A task that `now` ends at once when it can, and that `later` makes a body
/// for when `now` would wait. `payload` is for the two to share.
template <class Now, class Later>
dftu_task* make_abi_task(dstd::Vector<std::byte> payload, Now&& now,
                         Later&& later) {
    auto task = dstd::make_unique<AbiTask>();
    task->fast = dstd::make_unique<AbiFast>();
    task->fast->payload = std::move(payload);
    task->fast->now = dstd::Function<bool(AbiTask*)>(std::forward<Now>(now));
    task->fast->later = dstd::Function<coro::CoroTask<void>(AbiTask*)>(
        std::forward<Later>(later));
    return reinterpret_cast<dftu_task*>(task.release());
}

/// A task over a body that cannot fail.
dftu_task* task_to_abi(coro::CoroTask<void>&& body);

inline AbiTask* abi_task(dftu_task* task) noexcept {
    return reinterpret_cast<AbiTask*>(task);
}

/// Awaits the body once and gives the status; later calls give the same
/// status without waiting. A stop gives DFTU_E_CANCELLED and sets `stopped`.
coro::CoroTask<dftu_status> run_abi_task(AbiTask* task);

/// The error of a finished task with a negative status.
dstd::Error abi_task_error(const AbiTask& task);

/// Asks the task to stop and returns at once; the task still ends once, as
/// cancelled, with its own status or with OK. A finished task ignores it. The
/// caller holds a reference, the owner's or one from @ref retain_abi_task, so
/// the task cannot be freed under the call.
void cancel_abi_task(dftu_task* task) noexcept;

/// Adds a reference for a thread that will call @ref cancel_abi_task or
/// @ref abi_task_finished on a task another thread may free. The caller holds
/// a reference already, and drops the new one with @ref free_abi_task.
void retain_abi_task(dftu_task* task) noexcept;

/// Whether the task has finished.
bool abi_task_finished(const dftu_task* task) noexcept;

/// Drops one reference; the last one deletes the task. The awaiter that
/// finishes with a task drops the owner's reference this way.
void free_abi_task(dftu_task* task) noexcept;

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_TASK_ABI_H
