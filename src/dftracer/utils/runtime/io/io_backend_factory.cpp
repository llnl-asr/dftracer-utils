#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/runtime/io/io_backend_factory.h>
#include <dftracer/utils/runtime/io/thread_pool_backend.h>

#include <string_view>
#ifdef __linux__
#include <dftracer/utils/runtime/io/epoll_thread_pool_backend.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <dftracer/utils/runtime/io/kqueue_thread_pool_backend.h>
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
#include <dftracer/utils/runtime/io/io_uring_backend.h>
#endif

namespace dftracer::utils::io {

namespace {

constexpr bool HAVE_EPOLL =
#ifdef __linux__
    true;
#else
    false;
#endif

constexpr bool HAVE_KQUEUE =
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    true;
#else
    false;
#endif

constexpr bool HAVE_IO_URING =
#ifdef DFTRACER_UTILS_HAVE_IO_URING
    true;
#else
    false;
#endif

[[noreturn]] void fail_backend_env(std::string_view value,
                                   std::string_view why) {
    throw DFTUtilsException(
        ErrorCode::InvalidArgument,
        str_cat(constants::io_settings::BACKEND_ENV, ": '", value, "' ", why));
}

// Auto when the variable is unset, empty or "auto". Otherwise the backend it
// names; one the platform lacks or an unknown name throws.
IoBackendType backend_from_env(bool& forced) {
    forced = false;
    auto raw = Env::get<std::string_view>(constants::io_settings::BACKEND_ENV);
    if (!raw || raw->empty() || *raw == "auto") return IoBackendType::Auto;
    forced = true;
    if (*raw == "threadpool") return IoBackendType::Threadpool;
    if (*raw == "epoll") {
        if (!HAVE_EPOLL) fail_backend_env(*raw, "is not available here");
        return IoBackendType::EpollThreadpool;
    }
    if (*raw == "kqueue") {
        if (!HAVE_KQUEUE) fail_backend_env(*raw, "is not available here");
        return IoBackendType::KqueueThreadpool;
    }
    if (*raw == "io_uring") {
        if (!HAVE_IO_URING) fail_backend_env(*raw, "is not available here");
        return IoBackendType::IoUring;
    }
    fail_backend_env(*raw,
                     "is not a backend (use auto, io_uring, epoll, kqueue or "
                     "threadpool)");
}

}  // namespace

std::unique_ptr<IoBackend> create_io_backend(Executor& executor,
                                             std::size_t pool_size,
                                             IoBackendType backend_type,
                                             unsigned batch_threshold) {
    bool forced = false;
    if (backend_type == IoBackendType::Auto) {
        backend_type = backend_from_env(forced);
    }

    // Explicit backend selection (non-AUTO).
    if (backend_type == IoBackendType::Threadpool) {
        DFTRACER_UTILS_LOG_INFO("I/O backend: using threadpool (%zu threads)",
                                pool_size);
        return std::make_unique<ThreadPoolBackend>(executor, pool_size,
                                                   batch_threshold);
    }

#ifdef __linux__
    if (backend_type == IoBackendType::EpollThreadpool) {
        DFTRACER_UTILS_LOG_INFO(
            "I/O backend: using epoll+threadpool (%zu threads)", pool_size);
        return std::make_unique<EpollThreadPoolBackend>(executor, pool_size,
                                                        batch_threshold);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    if (backend_type == IoBackendType::KqueueThreadpool) {
        DFTRACER_UTILS_LOG_INFO(
            "I/O backend: using kqueue+threadpool (%zu threads)", pool_size);
        return std::make_unique<KqueueThreadPoolBackend>(executor, pool_size,
                                                         batch_threshold);
    }
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
    if (backend_type == IoBackendType::IoUring) {
        auto uring =
            std::make_unique<IoUringBackend>(executor, 256, batch_threshold);
        if (uring->probe()) {
            DFTRACER_UTILS_LOG_INFO("%s", "I/O backend: using io_uring");
            return uring;
        }
        if (forced) {
            fail_backend_env("io_uring", "failed its runtime probe");
        }
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "io_uring selected but runtime probe failed");
        // Fall through to AUTO detection.
    }
#endif

// Under Valgrind (<3.23.0, e.g. Ubuntu 24.04 ships 3.22.0), signals are not
// delivered inside io_uring_enter
// (https://bugs.kde.org/show_bug.cgi?id=428364), so Valgrind's scheduler cannot
// preempt the completion thread, deadlocking executor startup. Fall back to
// epoll, which handles signals correctly.
#if defined(DFTRACER_UTILS_HAVE_IO_URING) && \
    !defined(DFTRACER_UTILS_VALGRIND_MODE)
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
#elif defined(DFTRACER_UTILS_HAVE_IO_URING)
    DFTRACER_UTILS_LOG_INFO("%s",
                            "I/O backend: skipping io_uring (Valgrind mode)");
#endif

#ifdef DFTRACER_UTILS_VALGRIND_MODE
    // The epoll/kqueue completion thread only waits for shutdown today, so
    // under Valgrind use the plain thread pool to avoid an extra instrumented
    // thread and poll fd per executor.
    DFTRACER_UTILS_LOG_INFO(
        "I/O backend: using threadpool (Valgrind mode, %zu threads)",
        pool_size);
    return std::make_unique<ThreadPoolBackend>(executor, pool_size,
                                               batch_threshold);
#elif defined(__linux__)
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
