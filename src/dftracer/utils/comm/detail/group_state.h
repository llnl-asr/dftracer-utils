// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_DETAIL_GROUP_STATE_H
#define DFTRACER_UTILS_COMM_DETAIL_GROUP_STATE_H

#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::detail {

/// The protocol version the members of a group must agree on at the handshake.
inline constexpr std::uint32_t PROTOCOL_VERSION = 1;

/// Which collective a call makes. Two ranks that arrive at one sequence number
/// must name the same one.
enum class CallKind : std::uint8_t { Barrier, MonitoredBarrier, Split };

dstd::StringView call_name(CallKind kind) noexcept;

struct GroupState;

/**
 * @brief What the timeout of one call holds: the call and its group, and
 * nothing else.
 *
 * The timer entry keeps this one small block until the deadline, so the blocks
 * of a finished call are freed when it ends and not at its deadline. A callback
 * copies both pointers while they stand, which keeps them alive while it runs;
 * @ref clear empties them, and a callback that runs after that does nothing.
 */
struct TimerTarget {
    /// Gives the call and the group to a callback, or false once the call
    /// ended.
    bool take(dstd::SharedPtr<GroupState>& out_group,
              dstd::SharedPtr<PendingOp>& out_op) noexcept;

    /// Drops both, so a callback that has not taken them does nothing.
    void clear() noexcept;

    std::mutex mutex;
    dstd::SharedPtr<GroupState> group;
    dstd::SharedPtr<PendingOp> op;
};

/// The backend a communicator runs on, shared with the groups it made.
struct Backend {
    std::uint32_t ranks = 0;
    coro::Executor* executor = nullptr;
    coro::TimerService* timers = nullptr;
    /// The version each rank reports at the handshake, by world rank.
    dstd::Vector<std::uint32_t> protocol_version;
    /// The id the next group made on this backend takes.
    std::atomic<std::uint64_t> next_group_id{1};
};

/**
 * @brief One rendezvous: the calls the ranks made at one sequence number.
 *
 * Written and read only under the mutex of its @ref GroupState.
 */
struct Slot {
    explicit Slot(std::size_t size)
        : has_arrived(size, 0),
          ops(size),
          color(size, 0),
          key(size, 0),
          version(size, 0) {}

    CallKind kind = CallKind::Barrier;
    std::uint32_t first_rank = 0;
    std::uint32_t arrived = 0;
    dstd::Vector<char> has_arrived;
    dstd::Vector<dstd::SharedPtr<PendingOp>> ops;
    dstd::Vector<std::uint32_t> color;
    dstd::Vector<std::uint32_t> key;
    dstd::Vector<std::uint32_t> version;
};

/**
 * @brief The state of one group, shared by its handles and its calls.
 *
 * @ref members holds the world ranks in group order, so group rank `r` is
 * world rank `members[r]`. Everything but @ref id, @ref members and
 * @ref backend is guarded by @ref mutex.
 */
struct GroupState {
    GroupState() noexcept;
    GroupState(const GroupState&) = delete;
    GroupState& operator=(const GroupState&) = delete;
    ~GroupState();

    dstd::SharedPtr<Backend> backend;
    std::uint64_t id = 0;
    dstd::Vector<std::uint32_t> members;
    /// The sequence number the next call of each group rank takes.
    dstd::Vector<std::uint64_t> next_seq;
    /// The timeout each group rank uses when a call names no deadline, from
    /// the moment that call is made.
    dstd::Vector<std::uint64_t> default_timeout_ns;
    std::mutex mutex;
    dstd::Map<std::uint64_t, dstd::SharedPtr<Slot>> slots;
    dstd::Optional<dstd::Error> aborted;

    std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(members.size());
    }
};

/// A group of @p members (world ranks, in group order) on @p backend, taking
/// the next id of that backend and no default timeout.
dstd::SharedPtr<GroupState> make_group_state(
    const dstd::SharedPtr<Backend>& backend,
    dstd::Vector<std::uint32_t> members);

/// Groups that are alive now, which the tests read to prove a finished call
/// pins no state.
std::size_t count_live_groups() noexcept;

}  // namespace dftracer::utils::comm::detail

#endif  // DFTRACER_UTILS_COMM_DETAIL_GROUP_STATE_H
