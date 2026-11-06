// SPDX-License-Identifier: MIT
#include <dftracer/utils/runtime/io/thread_pool_backend.h>

namespace dftracer::utils::runtime::io {

ThreadPoolBackend::ThreadPoolBackend(std::size_t pool_size,
                                     unsigned batch_threshold,
                                     IoPoolOptions pool_options)
    : ThreadPoolFileOps(pool_size, batch_threshold, pool_options) {}

void ThreadPoolBackend::start() { pool_.start(); }
void ThreadPoolBackend::stop() { pool_.stop(); }

dstd::String ThreadPoolBackend::name() const { return "threadpool"; }

}  // namespace dftracer::utils::runtime::io
