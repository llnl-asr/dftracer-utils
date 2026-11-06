// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_H

#include <dftracer/utils/runtime/io/backend.h>
#include <dftracer/utils/runtime/io/backend_type.h>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::runtime::io {

/// I/O backend of the executor driving the calling thread, or null when that
/// thread drives none or the one it drives has no backend. The io layer asks
/// this instead of @ref Executor::current, which names no I/O type.
IoBackend *current_backend() noexcept;

/// Records `owner` as the executor driving the calling thread with `backend`.
/// The record holds only while @ref Executor::current is `owner`, so a
/// scope that suppresses that executor falls back to blocking calls too. A
/// null `backend` clears it. Not inherited by threads the owner did not start.
void set_current_backend(const coro::Executor *owner,
                         IoBackend *backend) noexcept;

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_H
