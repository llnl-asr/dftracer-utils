// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_THREAD_POOL_BACKEND_H
#define DFTRACER_UTILS_RUNTIME_IO_THREAD_POOL_BACKEND_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>

#include <cstddef>

namespace dftracer::utils::runtime::io {

/// Pure thread pool I/O backend -- universal fallback.
/// Runs every I/O operation on a small dedicated thread pool. The op
/// dispatch lives in ThreadPoolFileOps; this class adds only pool lifecycle
/// and the backend name (there is no reactor).
class ThreadPoolBackend : public ThreadPoolFileOps {
   public:
    explicit ThreadPoolBackend(std::size_t pool_size = 4,
                               unsigned batch_threshold = 0,
                               IoPoolOptions pool_options = {});

    void start() override;
    void stop() override;
    dstd::String name() const override;
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_THREAD_POOL_BACKEND_H
