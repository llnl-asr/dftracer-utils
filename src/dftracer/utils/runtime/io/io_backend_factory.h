// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_FACTORY_H
#define DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_FACTORY_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_thread_pool.h>

#include <cstddef>
#include <memory>

namespace dftracer::utils::runtime::io {

/// Create the best available I/O backend for this system.
/// @param pool_size         Most threads the I/O thread pool runs.
/// @param backend_type      Backend selection (AUTO = runtime detection).
/// @param batch_threshold   Waiting operations that wake a pool thread.
/// @param pool_options      Whether the pool starts full and how long an idle
///                          thread lives.
dstd::UniquePtr<IoBackend> make_io_backend(
    std::size_t pool_size = 4, IoBackendType backend_type = IoBackendType::Auto,
    unsigned batch_threshold = 16, IoPoolOptions pool_options = {});

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_IO_BACKEND_FACTORY_H
