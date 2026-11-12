// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_STOP_TOKEN_H
#define DFTRACER_UTILS_CORO_STOP_TOKEN_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <mutex>

namespace dftracer::utils::coro {

class Executor;
class StopSource;

/// Reads whether a @ref StopSource asked to stop. Copyable, and valid after the
/// source is gone. An empty token never stops.
class StopToken {
   public:
    StopToken() = default;

    bool stop_requested() const noexcept;

   private:
    friend class StopSource;
    struct State {
        std::atomic<bool> stopped{false};
        std::mutex mutex;
        dstd::Vector<dstd::WeakPtr<State>> children;
    };

    explicit StopToken(dstd::SharedPtr<State> state) noexcept
        : state_(std::move(state)) {}

    dstd::SharedPtr<State> state_;
};

/**
 * @brief Asks every holder of its token to stop. A source made from a parent
 * token stops with the parent, so sources form a tree.
 *
 * Thread-safe. A stop is not an error: tasks linked with @ref with_stop_token
 * end as cancelled, and the rest read the token.
 */
class StopSource {
   public:
    StopSource();
    explicit StopSource(const StopToken& parent);

    StopToken token() const noexcept { return StopToken(state_); }

    /// Sets the stop and wakes the parked waiters of linked tasks on @p
    /// executor, by default the one running this thread. Idempotent.
    void request_stop(Executor* executor = nullptr) noexcept;

   private:
    dstd::SharedPtr<StopToken::State> state_;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_STOP_TOKEN_H
