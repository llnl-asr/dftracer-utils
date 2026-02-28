#ifndef DFTRACER_UTILS_SERVER_TCP_LISTENER_H
#define DFTRACER_UTILS_SERVER_TCP_LISTENER_H

#include <dftracer/utils/core/coro/task.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace dftracer::utils {
class CoroScope;
}

namespace dftracer::utils::server {

/// TCP listener that accepts connections and spawns per-connection
/// coroutine handlers using structured concurrency.
class TcpListener {
   public:
    TcpListener(const std::string& addr, uint16_t port);
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// Bind and listen. Returns true on success.
    bool start(int backlog = 128);

    /// Accept loop: yields new client fds via coroutine.
    /// Runs until stop() is called or the listen socket is closed.
    /// Spawns handler(client_fd, addr) for each accepted connection.
    using ConnectionHandler = std::function<coro::CoroTask<void>(
        int client_fd, struct sockaddr_in addr)>;
    coro::CoroTask<void> accept_loop(CoroScope& scope,
                                     ConnectionHandler handler);

    /// Request graceful shutdown. The accept loop will break on the
    /// next iteration. In-flight handlers drain via CoroScope.
    void stop();

    int fd() const { return listen_fd_; }
    uint16_t port() const { return port_; }

   private:
    std::string bind_addr_;
    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> stopped_{false};
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_TCP_LISTENER_H
