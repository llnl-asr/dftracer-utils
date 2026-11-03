// SPDX-License-Identifier: MIT
#include <arpa/inet.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/tcp_listener.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>

namespace dftracer::utils::server {

TcpListener::TcpListener(const dstd::String& addr, uint16_t port)
    : bind_addr_(addr), port_(port) {}

TcpListener::~TcpListener() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool TcpListener::start(int backlog) {
#ifdef SOCK_CLOEXEC
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (listen_fd_ < 0) {
        DFTRACER_UTILS_LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }
#ifndef SOCK_CLOEXEC
    ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
#endif

    // Allow port reuse for quick restarts.
    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (bind_addr_ == "0.0.0.0" || bind_addr_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
            DFTRACER_UTILS_LOG_ERROR("Invalid bind address: %s",
                                     bind_addr_.c_str());
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        DFTRACER_UTILS_LOG_ERROR("bind(%s:%u) failed: %s", bind_addr_.c_str(),
                                 port_, std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, backlog) < 0) {
        DFTRACER_UTILS_LOG_ERROR("listen() failed: %s", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    return true;
}

coro::CoroTask<void> TcpListener::accept_loop(coro::Scope& scope,
                                              ConnectionHandler handler) {
    // Connections run in the caller's scope and can outlive this frame, so
    // each one shares ownership of the handler.
    const auto shared_handler =
        dstd::make_shared<const ConnectionHandler>(std::move(handler));
    while (!stopped_.load(std::memory_order_acquire) &&
           !g_shutdown_requested.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);

        ssize_t client_fd = co_await io::accept(
            listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
            &addrlen);

        if (client_fd < 0) {
            if (client_fd == -EINTR || client_fd == -EAGAIN) continue;
            // Listener stopped or fatal error
            break;
        }

        int fd = static_cast<int>(client_fd);

        // Ensure the client socket is blocking so recv/send work
        // properly in the IO thread pool.
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

        struct sockaddr_in addr_copy = client_addr;
        track_active(fd);
        scope.spawn([this, fd, addr_copy,
                     handler = shared_handler]() -> coro::CoroTask<void> {
            // One failed connection must not end the server, so its error is
            // logged here instead of reaching the scope's join.
            try {
                co_await (*handler)(fd, addr_copy);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("connection handler failed: %s",
                                         e.what());
            } catch (...) {
                DFTRACER_UTILS_LOG_ERROR("%s", "connection handler failed");
            }
            co_await io::close(fd);
            untrack_active(fd);
        });
    }

    // Shutdown requested: unblock any handler parked in recv on a keep-alive
    // connection so the spawned coroutines return and the scope can join.
    shutdown_active();

    // The signal handler may have already closed listen_fd_ via
    // g_listen_fd.  Mark it as -1 to prevent a double-close in the
    // destructor or stop().
    if (g_listen_fd.load(std::memory_order_acquire) < 0) {
        listen_fd_ = -1;
    }
}

void TcpListener::track_active(int fd) {
    std::lock_guard<std::mutex> lock(active_mu_);
    active_fds_.insert(fd);
}

void TcpListener::untrack_active(int fd) {
    std::lock_guard<std::mutex> lock(active_mu_);
    active_fds_.erase(fd);
}

void TcpListener::shutdown_active() {
    std::lock_guard<std::mutex> lock(active_mu_);
    for (int fd : active_fds_) {
        // Wakes a blocked recv/send in the handler; the handler then closes fd.
        ::shutdown(fd, SHUT_RDWR);
    }
}

void TcpListener::stop() {
    stopped_.store(true, std::memory_order_release);
    // Close the listen socket to unblock any pending accept.
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

}  // namespace dftracer::utils::server
