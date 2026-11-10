// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LONG_LIVED_RELEASE_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LONG_LIVED_RELEASE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/spawn_detached.h>

namespace dftracer::utils::utilities::fileio {

namespace detail {

template <typename Impl>
coro::CoroTask<void> join_and_delete(Impl* impl) {
    auto joining = impl->join_and_clean();
    auto settled = co_await coro::settle(std::move(joining));
    if (settled.exception) {
        const dstd::Error error = common::error_of_exception(settled.exception);
        DFTRACER_UTILS_LOG_ERROR("long-lived scope cleanup failed: {}",
                                 dstd::String(error.message()));
    }
    dstd::UniquePtr<Impl> owner(impl);
}

}  // namespace detail

/// Releases a writer's long-lived scope state. `Impl` provides settled(),
/// abandon() and join_and_clean(). A settled Impl is destroyed. Off an executor
/// thread the call blocks to join, else (or if that fails) a joiner task takes
/// the Impl. With no runtime left it leaks, as destroying frames terminates.
template <typename Impl>
void release_long_lived(dstd::UniquePtr<Impl> impl, const char* name) {
    if (!impl || impl->settled()) return;
    impl->abandon();
    if (!coro::Executor::current()) {
        const auto joined = default_runtime().block_on(
            coro::named(name, [&]() -> coro::CoroTask<void> {
                co_await impl->join_and_clean();
            }()));
        if (joined) return;
    }
    coro::Executor* exec = coro::Executor::current();
    if (!exec) {
        if (auto rt = peek_default_runtime()) exec = rt->executor();
    }
    if (spawn_detached(exec, detail::join_and_delete<Impl>(impl.get()))) {
        (void)impl.release();
        return;
    }
    DFTRACER_UTILS_LOG_WARN(
        "{}: no runtime left to join the writer's scope; leaking its state",
        name);
    (void)impl.release();
}

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LONG_LIVED_RELEASE_H
