#ifndef DFTRACER_UTILS_CORE_IO_THREAD_POOL_BACKEND_H
#define DFTRACER_UTILS_CORE_IO_THREAD_POOL_BACKEND_H

#include <dftracer/utils/core/io/thread_pool_file_ops.h>

#include <cstddef>
#include <string>

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// Pure thread pool I/O backend -- universal fallback.
/// Runs every I/O operation on a small dedicated thread pool. All submit_*
/// plumbing lives in ThreadPoolFileOps; this class adds only pool lifecycle
/// and the backend name (there is no reactor).
class ThreadPoolBackend : public ThreadPoolFileOps {
   public:
    explicit ThreadPoolBackend(Executor& executor, std::size_t pool_size = 4,
                               unsigned batch_threshold = 0);

    void start() override;
    void stop() override;
    std::string name() const override;
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_THREAD_POOL_BACKEND_H
