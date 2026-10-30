#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LONG_LIVED_RELEASE_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LONG_LIVED_RELEASE_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>

#include <exception>
#include <memory>

namespace dftracer::utils::utilities::fileio {

namespace detail {

template <typename Impl>
coro::CoroTask<void> join_and_delete(Impl* impl) {
    try {
        co_await impl->join_and_clean();
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("long-lived scope cleanup failed: %s",
                                 e.what());
    }
    delete impl;
}

}  // namespace detail

/// Releases a writer's long-lived CoroScope state. `Impl` provides settled(),
/// abandon() and join_and_clean(). A settled Impl is destroyed. Off an executor
/// thread the call blocks to join, else (or if that throws) a joiner task takes
/// the Impl. With no runtime left it leaks, as destroying frames terminates.
template <typename Impl>
void release_long_lived(std::unique_ptr<Impl> impl, const char* name) {
    if (!impl || impl->settled()) return;
    impl->abandon();
    if (!Executor::current()) {
        try {
            default_runtime().run_blocking(
                name, [&](CoroScope&) -> coro::CoroTask<void> {
                    co_await impl->join_and_clean();
                });
            return;
        } catch (...) {
        }
    }
    Executor* exec = Executor::current();
    if (!exec) {
        if (auto rt = peek_default_runtime()) exec = rt->executor();
    }
    if (spawn_detached(exec, detail::join_and_delete<Impl>(impl.get()))) {
        (void)impl.release();
        return;
    }
    DFTRACER_UTILS_LOG_WARN(
        "%s: no runtime left to join the writer's scope; leaking its state",
        name);
    (void)impl.release();
}

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LONG_LIVED_RELEASE_H
