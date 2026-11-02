// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_FACTORY_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_FACTORY_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_thread_pool.h>

#include <cstddef>
#include <memory>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::io {

/// Create the best available I/O backend for this system.
/// @param executor          The executor that owns this backend.
/// @param pool_size         Most threads the I/O thread pool runs.
/// @param backend_type      Backend selection (AUTO = runtime detection).
/// @param batch_threshold   SQE batch threshold (io_uring), 16 default.
/// @param pool_options      Whether the pool starts full and how long an idle
///                          thread lives, for the thread-pool backends.
dstd::UniquePtr<IoBackend> create_io_backend(
    coro::Executor& executor, std::size_t pool_size = 4,
    IoBackendType backend_type = IoBackendType::Auto,
    unsigned batch_threshold = 16, IoPoolOptions pool_options = {});

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_FACTORY_H
