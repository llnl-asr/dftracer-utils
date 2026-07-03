#ifdef __linux__

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/epoll_thread_pool_backend.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dftracer::utils::io {

EpollThreadPoolBackend::EpollThreadPoolBackend(Executor& executor,
                                               std::size_t pool_size,
                                               unsigned batch_threshold)
    : ThreadPoolFileOps(executor, pool_size, batch_threshold) {}

EpollThreadPoolBackend::~EpollThreadPoolBackend() {
    // Ensure cleanup even if stop() was not called.
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

void EpollThreadPoolBackend::start() {
    pool_.start();

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        DFTRACER_UTILS_LOG_ERROR("epoll_create1 failed: %s",
                                 std::strerror(errno));
        return;
    }

    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        DFTRACER_UTILS_LOG_ERROR("eventfd failed: %s", std::strerror(errno));
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // Register the eventfd with epoll for shutdown wakeup.
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = event_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0) {
        DFTRACER_UTILS_LOG_ERROR("epoll_ctl(eventfd) failed: %s",
                                 std::strerror(errno));
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "epoll+threadpool backend started (epoll_fd=%d, event_fd=%d)",
        epoll_fd_, event_fd_);

    completion_thread_.start([this] { epoll_loop(); });
}

void EpollThreadPoolBackend::stop() {
    // Signal the completion thread to exit.
    completion_thread_.signal_stop();

    // Wake epoll_wait by writing to eventfd.
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
    }

    completion_thread_.join();
    pool_.stop();

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

void EpollThreadPoolBackend::epoll_loop() {
    constexpr int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (completion_thread_.running()) {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == event_fd_) {
                // Shutdown signal -- drain the eventfd and exit.
                uint64_t val = 0;
                [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
                // Don't break immediately; process any other events first.
                continue;
            }

            // Future: dispatch socket I/O events here.
            // For now, only the eventfd is registered.
        }
    }
}

}  // namespace dftracer::utils::io

#endif  // __linux__
