#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io_backend_factory.h>
#include <dftracer/utils/core/io/thread_pool_backend.h>
#ifdef __linux__
#include <dftracer/utils/core/io/epoll_thread_pool_backend.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <dftracer/utils/core/io/kqueue_thread_pool_backend.h>
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
#include <dftracer/utils/core/io/io_uring_backend.h>
#endif

namespace dftracer::utils::io {

std::unique_ptr<IoBackend> create_io_backend(Executor& executor,
                                             std::size_t pool_size,
                                             IoBackendType backend_type,
                                             unsigned batch_threshold) {
    // Explicit backend selection (non-AUTO).
    if (backend_type == IoBackendType::THREADPOOL) {
        DFTRACER_UTILS_LOG_INFO("I/O backend: using threadpool (%zu threads)",
                                pool_size);
        return std::make_unique<ThreadPoolBackend>(executor, pool_size,
                                                   batch_threshold);
    }

#ifdef __linux__
    if (backend_type == IoBackendType::EPOLL_THREADPOOL) {
        DFTRACER_UTILS_LOG_INFO(
            "I/O backend: using epoll+threadpool (%zu threads)", pool_size);
        return std::make_unique<EpollThreadPoolBackend>(executor, pool_size,
                                                        batch_threshold);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    if (backend_type == IoBackendType::KQUEUE_THREADPOOL) {
        DFTRACER_UTILS_LOG_INFO(
            "I/O backend: using kqueue+threadpool (%zu threads)", pool_size);
        return std::make_unique<KqueueThreadPoolBackend>(executor, pool_size,
                                                         batch_threshold);
    }
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
    if (backend_type == IoBackendType::IO_URING) {
        auto uring =
            std::make_unique<IoUringBackend>(executor, 256, batch_threshold);
        if (uring->probe()) {
            DFTRACER_UTILS_LOG_INFO("%s", "I/O backend: using io_uring");
            return uring;
        }
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "io_uring selected but runtime probe failed");
        // Fall through to AUTO detection.
    }
#endif

    // AUTO detection: io_uring > epoll/kqueue+threadpool > threadpool.
#ifdef DFTRACER_UTILS_HAVE_IO_URING
    {
        auto uring =
            std::make_unique<IoUringBackend>(executor, 256, batch_threshold);
        if (uring->probe()) {
            DFTRACER_UTILS_LOG_INFO("%s", "I/O backend: using io_uring");
            return uring;
        }
        DFTRACER_UTILS_LOG_INFO("%s",
                                "io_uring runtime probe failed, falling back");
    }
#endif

#ifdef __linux__
    DFTRACER_UTILS_LOG_INFO("I/O backend: using epoll+threadpool (%zu threads)",
                            pool_size);
    return std::make_unique<EpollThreadPoolBackend>(executor, pool_size,
                                                    batch_threshold);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    DFTRACER_UTILS_LOG_INFO(
        "I/O backend: using kqueue+threadpool (%zu threads)", pool_size);
    return std::make_unique<KqueueThreadPoolBackend>(executor, pool_size,
                                                     batch_threshold);
#else
    DFTRACER_UTILS_LOG_INFO("I/O backend: using threadpool (%zu threads)",
                            pool_size);
    return std::make_unique<ThreadPoolBackend>(executor, pool_size,
                                               batch_threshold);
#endif
}

}  // namespace dftracer::utils::io
