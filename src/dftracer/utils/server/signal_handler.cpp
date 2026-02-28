#include <dftracer/utils/server/signal_handler.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstring>

namespace dftracer::utils::server {

std::atomic<bool> g_shutdown_requested{false};
std::atomic<int> g_listen_fd{-1};

static void signal_handler(int /*sig*/) {
    g_shutdown_requested.store(true, std::memory_order_release);
    int fd = g_listen_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        // shutdown() interrupts a blocked accept() on Linux.
        // close() interrupts a blocked accept() on macOS.
        // Both are async-signal-safe.  Use both for portability.
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

void install_signal_handlers() {
    // Use sigaction instead of std::signal so the handler is NOT
    // reset to SIG_DFL after the first invocation (SA_RESETHAND is
    // not set) and SA_RESTART is not set so that blocked syscalls
    // like accept() return EINTR.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART — accept() must return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

}  // namespace dftracer::utils::server