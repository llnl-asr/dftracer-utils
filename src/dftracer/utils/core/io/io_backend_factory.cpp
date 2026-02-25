#include "io_backend_factory.h"

#include <dftracer/utils/core/common/logging.h>

#include "thread_pool_backend.h"
#ifdef __linux__
#include "epoll_thread_pool_backend.h"
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include "kqueue_thread_pool_backend.h"
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
#include "io_uring_backend.h"
#endif

namespace dftracer::utils::io {

std::unique_ptr<IoBackend> create_io_backend(Executor& executor,
                                             std::size_t pool_size,
                                             IoBackendType backend_type,
                                             unsigned batch_threshold) {
    // Explicit backend selection (non-AUTO).
    if (backend_type == IoBackendType::THREADPOOL) {
        DFTRACER_UTILS_LOG_DEBUG(
            "I/O backend: using threadpool (%zu threads, forced)", pool_size);
        return std::make_unique<ThreadPoolBackend>(executor, pool_size,
                                                   batch_threshold);
    }

#ifdef __linux__
    if (backend_type == IoBackendType::EPOLL_THREADPOOL) {
        DFTRACER_UTILS_LOG_DEBUG(
            "I/O backend: using epoll+threadpool (%zu threads, forced)",
            pool_size);
        return std::make_unique<EpollThreadPoolBackend>(executor, pool_size,
                                                        batch_threshold);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    if (backend_type == IoBackendType::KQUEUE_THREADPOOL) {
        DFTRACER_UTILS_LOG_DEBUG(
            "I/O backend: using kqueue+threadpool (%zu threads, forced)",
            pool_size);
        return std::make_unique<KqueueThreadPoolBackend>(executor, pool_size,
                                                         batch_threshold);
    }
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
    if (backend_type == IoBackendType::IO_URING) {
        auto uring =
            std::make_unique<IoUringBackend>(executor, 256, batch_threshold);
        if (uring->probe()) {
            DFTRACER_UTILS_LOG_DEBUG("%s",
                                     "I/O backend: using io_uring (forced)");
            return uring;
        }
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "io_uring forced but runtime probe failed");
        // Fall through to AUTO detection.
    }
#endif

    // AUTO detection: io_uring > epoll/kqueue+threadpool > threadpool.
#ifdef DFTRACER_UTILS_HAVE_IO_URING
    {
        auto uring =
            std::make_unique<IoUringBackend>(executor, 256, batch_threshold);
        if (uring->probe()) {
            DFTRACER_UTILS_LOG_DEBUG("%s", "I/O backend: using io_uring");
            return uring;
        }
        DFTRACER_UTILS_LOG_DEBUG("%s",
                                 "io_uring runtime probe failed, falling back");
    }
#endif

#ifdef __linux__
    DFTRACER_UTILS_LOG_DEBUG(
        "I/O backend: using epoll+threadpool (%zu threads)", pool_size);
    return std::make_unique<EpollThreadPoolBackend>(executor, pool_size,
                                                    batch_threshold);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    DFTRACER_UTILS_LOG_DEBUG(
        "I/O backend: using kqueue+threadpool (%zu threads)", pool_size);
    return std::make_unique<KqueueThreadPoolBackend>(executor, pool_size,
                                                     batch_threshold);
#else
    DFTRACER_UTILS_LOG_DEBUG("I/O backend: using threadpool (%zu threads)",
                             pool_size);
    return std::make_unique<ThreadPoolBackend>(executor, pool_size,
                                               batch_threshold);
#endif
}

}  // namespace dftracer::utils::io
