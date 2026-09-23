#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/kqueue_thread_pool_backend.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dftracer::utils::io {

KqueueThreadPoolBackend::KqueueThreadPoolBackend(Executor& executor,
                                                 std::size_t pool_size,
                                                 unsigned batch_threshold)
    : ThreadPoolFileOps(executor, pool_size, batch_threshold) {}

KqueueThreadPoolBackend::~KqueueThreadPoolBackend() = default;

void KqueueThreadPoolBackend::start() {
    pool_.start();

    kqueue_fd_.reset(::kqueue());
    if (!kqueue_fd_.valid()) {
        DFTRACER_UTILS_LOG_ERROR("kqueue() failed: %s", std::strerror(errno));
        return;
    }

    // Register a user event (EVFILT_USER) for shutdown signaling.
    struct kevent ev{};
    EV_SET(&ev, SHUTDOWN_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kqueue_fd_.get(), &ev, 1, nullptr, 0, nullptr) < 0) {
        DFTRACER_UTILS_LOG_ERROR("kevent(register EVFILT_USER) failed: %s",
                                 std::strerror(errno));
    }

    DFTRACER_UTILS_LOG_DEBUG("kqueue+threadpool backend started (kqueue_fd=%d)",
                             kqueue_fd_.get());

    completion_thread_.start([this] { kqueue_loop(); });
}

void KqueueThreadPoolBackend::stop() {
    // Signal the completion thread to exit.
    completion_thread_.signal_stop();

    // Wake kevent() by triggering the user event.
    if (kqueue_fd_.valid()) {
        struct kevent ev{};
        EV_SET(&ev, SHUTDOWN_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(kqueue_fd_.get(), &ev, 1, nullptr, 0, nullptr);
    }

    completion_thread_.join();
    pool_.stop();

    kqueue_fd_.reset();
}

void KqueueThreadPoolBackend::kqueue_loop() {
    constexpr int MAX_EVENTS = 64;
    struct kevent events[MAX_EVENTS];

    // 100ms timeout for periodic running() check.
    struct timespec timeout{};
    timeout.tv_sec = 0;
    timeout.tv_nsec = 100 * 1000 * 1000;  // 100ms

    while (completion_thread_.running()) {
        int n = ::kevent(kqueue_fd_.get(), nullptr, 0, events, MAX_EVENTS,
                         &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_USER &&
                events[i].ident == SHUTDOWN_IDENT) {
                // Shutdown signal -- process any other events first.
                continue;
            }

            // Future: dispatch socket I/O events here.
            // For now, only the user event is registered.
        }
    }
}

}  // namespace dftracer::utils::io

#endif  // kqueue platforms
