// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_SERVER_TCP_LISTENER_H
#define DFTRACER_UTILS_SERVER_TCP_LISTENER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::coro {
class Scope;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::server {

/// TCP listener that accepts connections and spawns per-connection
/// coroutine handlers using structured concurrency.
class TcpListener {
   public:
    TcpListener(const dstd::String& addr, uint16_t port);
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// Bind and listen. Returns true on success.
    bool start(int backlog = 128);

    /// Accept loop: yields new client fds via coroutine.
    /// Runs until stop() is called or the listen socket is closed.
    /// Spawns handler(client_fd, addr) for each accepted connection.
    using ConnectionHandler = dstd::Function<coro::CoroTask<void>(
        int client_fd, struct sockaddr_in addr)>;
    coro::CoroTask<void> accept_loop(coro::Scope& scope,
                                     ConnectionHandler handler);

    /// Request graceful shutdown. The accept loop will break on the
    /// next iteration. In-flight handlers drain via @ref
    /// dftracer::utils::coro::Scope "Scope".
    void stop();

    int fd() const { return listen_fd_; }
    uint16_t port() const { return port_; }

   private:
    /// Track live client fds so shutdown can interrupt handlers parked in recv
    /// on keep-alive connections (otherwise the accept loop breaks but the
    /// spawned handlers never return and the scope never joins).
    void track_active(int fd);
    void untrack_active(int fd);
    void shutdown_active();

    dstd::String bind_addr_;
    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> stopped_{false};
    std::mutex active_mu_;
    dstd::HashSet<int> active_fds_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_TCP_LISTENER_H
