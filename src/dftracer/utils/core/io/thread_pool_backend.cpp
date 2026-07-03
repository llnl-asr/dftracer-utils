#include <dftracer/utils/core/io/thread_pool_backend.h>

namespace dftracer::utils::io {

ThreadPoolBackend::ThreadPoolBackend(Executor& executor, std::size_t pool_size,
                                     unsigned batch_threshold)
    : ThreadPoolFileOps(executor, pool_size, batch_threshold) {}

void ThreadPoolBackend::start() { pool_.start(); }
void ThreadPoolBackend::stop() { pool_.stop(); }

std::string ThreadPoolBackend::name() const { return "threadpool"; }

}  // namespace dftracer::utils::io
