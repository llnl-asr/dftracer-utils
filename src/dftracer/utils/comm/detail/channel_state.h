// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_DETAIL_CHANNEL_STATE_H
#define DFTRACER_UTILS_COMM_DETAIL_CHANNEL_STATE_H

#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace dftracer::utils::comm::detail {

struct Backend;
struct GroupState;

/// The one declaration each end of a channel makes to a peer: the id it opened
/// and the bytes of one of its values. A peer that opened another channel sees
/// it here instead of waiting for a message that can never arrive.
struct ChannelOpen {
    std::uint64_t channel = 0;
    std::uint32_t value_size = 0;
    std::uint32_t reserved = 0;
};

/// A peer that has not declared a channel yet, and the declarations of it the
/// hub had counted when it said so.
struct PeerWait {
    std::uint32_t peer = 0;
    std::uint64_t seen = 0;
};

/// One turn of @ref ChannelRx::recv_in_rank_order: the rank it waits for, and
/// the order the turn was handed out in.
struct RankTurn {
    std::uint32_t peer = 0;
    std::uint64_t stamp = 0;
};

/**
 * @brief What the channels of one group share: the declarations their peers
 * made, which whichever channel of the group receives from a peer first takes.
 *
 * A rank declares its opens to a peer on one stream per pair, so the
 * declarations of two channels of one pair arrive in one order and never wait
 * for a message of a channel the other end did not open. The hub holds the one
 * receive of that stream, so the calls that wait for a declaration of a peer
 * all wake when it comes, whichever channel it names.
 */
class ChannelHub : public std::enable_shared_from_this<ChannelHub> {
   public:
    /// What a receive knows of the open of its peer.
    enum class Open : std::uint8_t {
        /// The peer opened the channel with the value size this rank opened.
        Matched,
        /// The peer opened another channel, or another value size.
        Mismatch,
        /// The peer has not declared one of this rank's channels yet.
        Waiting,
    };

    /// Ends the wait for a declaration: @p why is null when one arrived, and
    /// valid during the call only.
    using Wake = dstd::Function<void(const dstd::Error* why)>;

    ChannelHub(dstd::SharedPtr<backend::Messaging> messaging,
               std::uint64_t group_id, std::uint32_t rank, std::uint32_t size);

    ChannelHub(const ChannelHub&) = delete;
    ChannelHub& operator=(const ChannelHub&) = delete;

    /// Notes that this rank opened @p channel, @p value_size bytes wide.
    void note_open(std::uint64_t channel, std::uint32_t value_size);

    /// Reads what @p peer declared of @p channel. A declaration that names it
    /// with another value size is a mismatch for every receive of the channel,
    /// now and later, and @p wrong names it. A declaration of a channel this
    /// rank has not opened is kept for when it does. @p seen gets the number of
    /// declarations of @p peer the answer is based on, for @ref watch.
    Open open(std::uint32_t peer, std::uint64_t channel, ChannelOpen& wrong,
              std::uint64_t& seen);

    /// A ticket no other wait of this hub has, for @ref watch and @ref unwatch.
    std::uint64_t make_ticket() noexcept;

    /// The ids @p peer declared that this rank never opened.
    dstd::Vector<std::uint64_t> unopened(std::uint32_t peer);

    /// Wakes @p wake once @p peer declares something, or the group ends. The
    /// wait is identified by @p ticket for @ref unwatch, and ends when @p call
    /// ends or is gone.
    /// A declaration that came after @p seen was read is not waited for: the
    /// wait is not registered and false is given, so the caller reads again.
    bool watch(std::uint32_t peer, std::uint64_t ticket,
               const dstd::WeakPtr<PendingOp>& call, Wake wake,
               std::uint64_t seen);

    /// Drops the wait @p ticket of @p peer, and gives whether it was still
    /// waiting: when it was not, it woke or is waking, and ends on its own.
    bool unwatch(std::uint32_t peer, std::uint64_t ticket);

   private:
    /// One call that waits for a declaration of one peer.
    struct Watcher {
        std::uint64_t ticket = 0;
        dstd::WeakPtr<PendingOp> call;
        Wake wake;
    };

    /// What the hub knows of one peer.
    struct Peer {
        /// The value size of each channel the peer declared, by channel id.
        dstd::Map<std::uint64_t, std::uint32_t> declared;
        dstd::Vector<Watcher> watchers;
        /// Whether the receive of its next declaration is parked.
        bool listening = false;
        /// How many declarations and failures of its stream the hub took.
        std::uint64_t generation = 0;
    };

    void declared(std::uint32_t peer, dstd::Span<const std::byte> bytes,
                  const dstd::Error* why);

    dstd::SharedPtr<backend::Messaging> messaging_;
    std::uint64_t group_id_ = 0;
    std::uint32_t rank_ = 0;
    std::atomic<std::uint64_t> tickets_{0};
    std::mutex mutex_;
    /// The bytes of one value of each channel this rank opened.
    dstd::Map<std::uint64_t, std::uint32_t> opened_;
    /// By group rank.
    dstd::Vector<Peer> peers_;
};

/**
 * @brief The state of one open channel, shared by its handles and its calls.
 *
 * The id of a channel is the FNV-1a hash of its name, so one name gives one id
 * on every rank. The streams of a channel hold the id in the index of their
 * tag, so no channel takes a message of another. @ref mutex guards everything
 * below the names; no provider completion runs under it.
 */
class ChannelState : public std::enable_shared_from_this<ChannelState> {
   public:
    ChannelState(dstd::SharedPtr<Backend> backend,
                 dstd::SharedPtr<backend::Messaging> messaging,
                 dstd::SharedPtr<ChannelHub> hub,
                 dstd::WeakPtr<GroupState> group, std::uint64_t group_id,
                 std::uint64_t id, std::uint32_t size, std::uint32_t rank,
                 dstd::StringView name, std::uint32_t value_size,
                 ChannelOpts opts);

    ChannelState(const ChannelState&) = delete;
    ChannelState& operator=(const ChannelState&) = delete;

    std::uint64_t id() const noexcept { return id_; }
    std::uint64_t group_id() const noexcept { return group_id_; }
    std::uint32_t rank() const noexcept { return rank_; }
    std::uint32_t size() const noexcept { return size_; }
    std::uint32_t value_size() const noexcept { return value_size_; }
    const dstd::String& name() const noexcept { return name_; }

    const dstd::SharedPtr<backend::Messaging>& messaging() const noexcept {
        return messaging_;
    }
    const dstd::SharedPtr<ChannelHub>& hub() const noexcept { return hub_; }
    const dstd::SharedPtr<Backend>& backend() const noexcept {
        return backend_;
    }

    /// The stream this rank sends the messages to @p peer on.
    backend::MessageKey send_key(std::uint32_t peer) const noexcept;
    /// The stream @p source sends the messages this rank receives on.
    backend::MessageKey recv_key(std::uint32_t source) const noexcept;
    /// The stream this rank declares its opens to @p peer on.
    backend::MessageKey declare_key(std::uint32_t peer) const noexcept;
    /// The depth and overflow of the stream a send of this rank takes.
    backend::StreamOpts stream_opts() const noexcept;

    /// The next peer of @ref ChannelRx::recv_in_rank_order: the ranks in order,
    /// this rank left out, from the first one on.
    RankTurn next_rank() noexcept;
    /// Gives back the turn of @p peer when the call that took it ended without
    /// a message, so the next call takes the oldest turn given back first.
    void return_rank(const RankTurn& turn) noexcept;

    /// Declares the open of this channel to @p peer, once.
    void declare(std::uint32_t peer);

    /// Whether the sending end of this rank is closed.
    bool tx_closed() const noexcept;
    /// Closes the sending end of the channel to every peer, so a send after it
    /// fails and every receiver sees the messages sent before it first.
    void close_sending();

    /// The timeout the calls of the group use when they name no deadline, or 0.
    std::uint64_t default_timeout() const noexcept;

   private:
    dstd::SharedPtr<Backend> backend_;
    dstd::SharedPtr<backend::Messaging> messaging_;
    dstd::SharedPtr<ChannelHub> hub_;
    dstd::WeakPtr<GroupState> group_;
    std::uint64_t group_id_ = 0;
    std::uint64_t id_ = 0;
    std::uint32_t size_ = 0;
    std::uint32_t rank_ = 0;
    dstd::String name_;
    std::uint32_t value_size_ = 0;
    ChannelOpts opts_;

    mutable std::mutex mutex_;
    /// One byte per peer: this rank declared its open of the channel to it.
    dstd::Vector<std::uint8_t> declared_;
    bool close_sent_ = false;
    /// Where @ref next_rank reads from.
    std::uint32_t cursor_ = 0;
    /// The stamp the next turn takes.
    std::uint64_t stamp_ = 0;
    /// The turns given back that no call took again.
    dstd::Vector<RankTurn> returned_;
};

/// Opens the channel @p name of @p group, which carries @p value_size bytes per
/// value, and gives nothing for a group of fewer than two ranks.
dstd::SharedPtr<ChannelState> open_channel(
    const dstd::SharedPtr<Backend>& backend,
    const dstd::SharedPtr<GroupState>& group, std::uint32_t rank,
    std::uint64_t id, dstd::StringView name, std::uint32_t value_size,
    ChannelOpts opts);

/// The error of a call of a channel that no group of two ranks opened.
dstd::Error empty_channel_error();

/// The error of a send or a receive of a rank the group does not have.
dstd::Error no_peer_error(const ChannelState& channel, std::uint32_t peer);

/// The error of a receive whose sender closed the channel, or whose senders all
/// did for @p peer of @ref backend::ANY_SOURCE.
dstd::Error closed_error(const ChannelState& channel, std::uint32_t peer);

/// The error of a send whose own rank closed the sending end.
dstd::Error closed_send_error(const ChannelState& channel);

/// The error of a send whose stream to @p peer is full.
dstd::Error full_error(const ChannelState& channel, std::uint32_t peer,
                       std::size_t depth);

/// The error of a receive whose peer opened another channel or another value
/// size, naming the channel and both ranks.
dstd::Error mismatch_error(const ChannelState& channel, std::uint32_t peer,
                           const ChannelOpen& opened);

/// @p why for a receive from @p peer, or from every peer for
/// @ref backend::ANY_SOURCE. A deadline that passed names the channels the peer
/// declared that this rank never opened, which is what a name mismatch looks
/// like; any other error is as it is.
dstd::Error with_hint(const ChannelState& channel, std::uint32_t peer,
                      dstd::Error why);

/// The error of a message whose size is not the value size of the channel.
dstd::Error size_error(const ChannelState& channel, std::uint32_t peer,
                       std::size_t bytes);

/// The error of a call the provider ended: a close of the sender becomes the
/// close of the channel, and any other error is the one that ended it, since a
/// full stream and a short buffer already name their channel. @p peer is
/// @ref backend::ANY_SOURCE for a call that waited on every sender.
dstd::Error gave_up(const ChannelState& channel, std::uint32_t peer,
                    const dstd::Error& why);

}  // namespace dftracer::utils::comm::detail

#endif  // DFTRACER_UTILS_COMM_DETAIL_CHANNEL_STATE_H
