// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_DETAIL_GROUP_STATE_H
#define DFTRACER_UTILS_COMM_DETAIL_GROUP_STATE_H

#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/comm/backend/messaging.h>
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
class ChannelHub;

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

    /// Gives the call of a channel to a callback, or false once the call ended.
    bool take_call(dstd::SharedPtr<PendingOp>& out_op) noexcept;

    /// Drops both, so a callback that has not taken them does nothing.
    void clear() noexcept;

    std::mutex mutex;
    dstd::SharedPtr<GroupState> group;
    dstd::SharedPtr<PendingOp> op;
};

/// The backend a communicator runs on, shared with the groups it made.
///
/// The providers are the capabilities the groups use, one of each per backend,
/// all three bound before a group makes a call.
struct Backend {
    dstd::SharedPtr<backend::Bootstrap> bootstrap;
    dstd::SharedPtr<backend::Messaging> messaging;
    dstd::SharedPtr<backend::Collectives> collectives;
    /// The version each rank reports at the handshake, by world rank.
    dstd::Vector<std::uint32_t> protocol_version;
    /// The id the next group made on this backend takes.
    std::atomic<std::uint64_t> next_group_id{1};

    std::uint32_t ranks() const noexcept { return bootstrap->ranks(); }
};

/**
 * @brief One rendezvous: the calls the ranks made at one sequence number.
 *
 * The records the members bring to the slot live in the @ref backend
 * provider, which reports the slot complete. Written and read only under the
 * mutex of its @ref GroupState.
 */
struct Slot {
    explicit Slot(std::uint64_t slot_seq, std::size_t size)
        : seq(slot_seq), ops(size) {}

    /// The group of the slot, so a completion that outlives the handles finds
    /// it. Weak, because the group holds its slots.
    dstd::WeakPtr<GroupState> group;
    std::uint64_t seq = 0;
    /// Set by the first completion of the slot, which is the only one that ends
    /// the calls of its members.
    std::atomic<bool> finished{false};
    dstd::Vector<dstd::SharedPtr<PendingOp>> ops;
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
    /// What the channels each group rank opened share, made by its first
    /// channel: what it opened, and what its peers declared to it.
    dstd::Vector<dstd::SharedPtr<ChannelHub>> channel_hubs;

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
