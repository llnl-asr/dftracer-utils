#ifndef DFTRACER_UTILS_SERVER_CANCEL_H
#define DFTRACER_UTILS_SERVER_CANCEL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dftracer::utils::server {

/// Shared state behind a CancelToken: the cancel flag plus, optionally, the
/// peer socket so a running handler can notice a client disconnect.
struct CancelState {
    std::atomic<bool> flag{false};
    int peer_fd = -1;
    std::atomic<std::uint32_t> probe_tick{0};
};

/// Cooperative cancellation handle for one in-flight request. Cheap to copy;
/// handlers check `cancelled()` at loop boundaries and bail out. A
/// default-constructed token is never cancelled.
class CancelToken {
   public:
    CancelToken() = default;
    explicit CancelToken(std::shared_ptr<CancelState> state)
        : state_(std::move(state)) {}

    /// True if cancellation was requested, or (when a peer fd is set) the
    /// client has disconnected. The disconnect check is a throttled
    /// non-blocking probe.
    bool cancelled() const;
    void cancel() const {
        if (state_) state_->flag.store(true, std::memory_order_relaxed);
    }
    explicit operator bool() const { return static_cast<bool>(state_); }

   private:
    std::shared_ptr<CancelState> state_;
};

/// Process-global map of request id -> cancel state. Requests register under a
/// client-supplied id for the duration of the handler; a separate cancel
/// request (on another connection) flips the flag by id.
class CancelRegistry {
   public:
    static CancelRegistry& instance();

    /// An empty id yields an unregistered token, still usable for
    /// disconnect-driven cancellation. `peer_fd` (>= 0) enables the disconnect
    /// probe.
    CancelToken create(const std::string& id, int peer_fd = -1);
    void remove(const std::string& id);
    bool cancel(const std::string& id);
    std::size_t size() const;

   private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<CancelState>> tokens_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_CANCEL_H
