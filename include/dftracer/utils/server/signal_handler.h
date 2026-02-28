#ifndef DFTRACER_UTILS_SERVER_SIGNAL_HANDLER_H
#define DFTRACER_UTILS_SERVER_SIGNAL_HANDLER_H

#include <atomic>

namespace dftracer::utils::server {

/// Global shutdown flag, set by the signal handler.
/// Check this in accept loops and long-running operations.
extern std::atomic<bool> g_shutdown_requested;

/// Global listen fd for the signal handler to close on shutdown.
/// Set this before entering the accept loop.
extern std::atomic<int> g_listen_fd;

/// Install SIGINT/SIGTERM handlers that set g_shutdown_requested
/// and shutdown the listen socket to unblock accept().
void install_signal_handlers();

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_SIGNAL_HANDLER_H
