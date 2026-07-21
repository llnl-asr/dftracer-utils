#include <dftracer/utils/server/cancel.h>
#include <dftracer/utils/server/signal_handler.h>
#include <sys/socket.h>

#include <cerrno>

namespace dftracer::utils::server {

namespace {

// Non-blocking peek: true if the peer has closed or reset the connection.
bool peer_disconnected(int fd) {
    char b;
    ssize_t n = ::recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return true;  // orderly shutdown (FIN)
    if (n < 0) {
        int e = errno;
        // "No data yet" vs. a real reset/error.
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        return e != EAGAIN && e != EWOULDBLOCK;
#else
        return e != EAGAIN;
#endif
    }
    return false;  // data buffered (e.g. a pipelined request): still connected
}

}  // namespace

bool CancelToken::cancelled() const {
    // Shutdown cancels everything, including work started without a token, so
    // Ctrl-C does not wait out a long scan.
    if (g_shutdown_requested.load(std::memory_order_acquire)) return true;
    if (!state_) return false;
    if (state_->flag.load(std::memory_order_relaxed)) return true;
    if (state_->peer_fd < 0) return false;
    // Probe the socket only every 16th check to keep the hot path cheap.
    if ((state_->probe_tick.fetch_add(1, std::memory_order_relaxed) & 0xF) != 0)
        return false;
    if (peer_disconnected(state_->peer_fd)) {
        state_->flag.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

CancelRegistry& CancelRegistry::instance() {
    static CancelRegistry registry;
    return registry;
}

CancelToken CancelRegistry::create(const std::string& id, int peer_fd) {
    auto state = std::make_shared<CancelState>();
    state->peer_fd = peer_fd;
    if (!id.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_[id] = state;
    }
    return CancelToken(state);
}

void CancelRegistry::remove(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    tokens_.erase(id);
}

bool CancelRegistry::cancel(const std::string& id) {
    if (id.empty()) return false;
    std::shared_ptr<CancelState> state;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tokens_.find(id);
        if (it == tokens_.end()) return false;
        state = it->second;
    }
    state->flag.store(true, std::memory_order_relaxed);
    return true;
}

std::size_t CancelRegistry::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tokens_.size();
}

}  // namespace dftracer::utils::server
