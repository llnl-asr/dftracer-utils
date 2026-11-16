// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_MESSAGING_H
#define DFTRACER_UTILS_COMM_BACKEND_MESSAGING_H

#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::comm::backend {

/// One stream of messages: the messages two ranks of a group send each other on
/// one tag, in the order their sender sent them.
///
/// The tag of a stream a collective uses holds its sequence number, so a
/// message of one call never answers the receive of another. @ref source of
/// @ref ANY_SOURCE names no sender and no stream: it is the source of a receive
/// that any sender answers.
///
/// Keys order by group, destination, tag and then source, so the streams of one
/// destination and tag lie together and a receive of any source finds them
/// without a look at the others.
struct MessageKey {
    std::uint64_t group = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint64_t tag = 0;

    friend bool operator<(const MessageKey& a, const MessageKey& b) noexcept {
        if (a.group != b.group) return a.group < b.group;
        if (a.destination != b.destination)
            return a.destination < b.destination;
        if (a.tag != b.tag) return a.tag < b.tag;
        return a.source < b.source;
    }
};

/// The source of a message that any member of the group may have sent.
inline constexpr std::uint32_t ANY_SOURCE = 0xFFFFFFFFu;

/// What a stream carries. The high bits of a tag hold it, so the streams of a
/// channel never meet those of a collective: a message of one is never taken by
/// a receive of the other.
enum class TagPurpose : std::uint8_t {
    CollectiveRecord = 0,
    CollectiveTable = 1,
    ChannelOpen = 2,
    ChannelMessage = 3,
    /// The requests a rank answers for the bulk transfers of its peers.
    BulkRequest = 4,
    /// What the answering end of one bulk transfer sends the other end.
    BulkReply = 5,
    /// The bytes the pushing end of one bulk transfer sends.
    BulkData = 6,
    /// The bytes the answering end of one pull sends.
    BulkPullData = 7,
};

inline constexpr std::uint64_t TAG_PURPOSE_SHIFT = 60;
inline constexpr std::uint64_t TAG_INDEX_MASK =
    (std::uint64_t{1} << TAG_PURPOSE_SHIFT) - 1;

/// The tag of the stream @p index of @p purpose.
constexpr std::uint64_t message_tag(TagPurpose purpose,
                                    std::uint64_t index) noexcept {
    return (static_cast<std::uint64_t>(purpose) << TAG_PURPOSE_SHIFT) |
           (index & TAG_INDEX_MASK);
}

/// What @p tag carries.
constexpr TagPurpose tag_purpose(std::uint64_t tag) noexcept {
    return static_cast<TagPurpose>(tag >> TAG_PURPOSE_SHIFT);
}

/// The index of @p tag in the space of its purpose.
constexpr std::uint64_t tag_index(std::uint64_t tag) noexcept {
    return tag & TAG_INDEX_MASK;
}

/// Identifies one parked send or receive, so its owner ends that call alone. A
/// send that its stream stored already ended, so it has no handle.
struct OpId {
    std::uint64_t value = 0;
};

/// The handle of a call that ended at once, which nothing can cancel.
inline constexpr OpId NO_OP{};

/// A depth of 0 holds every message its sender parks on a stream, which is what
/// the plain send of a collective does. A channel always names its depth.
inline constexpr std::size_t HOLD_ALL = 0;

/// What a stream holds for a message no receive took yet, and what a send does
/// when it holds that much.
struct StreamOpts {
    std::size_t depth = HOLD_ALL;
    Overflow overflow = Overflow::Block;
};

/// The stream of a send that parks every message, which is what a collective
/// wants: no message of it is refused or dropped by a depth.
inline constexpr StreamOpts HOLD_EVERYTHING{};

/**
 * @brief The bytes of one message: owned by it, or borrowed from a block that a
 * keep-alive holds.
 *
 * A borrowed payload makes a send copy nothing: its @ref bytes stay valid until
 * the message is delivered or dropped, because the message holds @p keep until
 * then. A caller that has an owned vector passes it as is.
 */
class Payload {
   public:
    Payload() noexcept = default;

    /// The message owns @p bytes.
    Payload(dstd::Vector<std::byte> bytes) noexcept  // NOLINT: owned form
        : owned_(std::move(bytes)), is_owned_(true) {}

    /// The message borrows @p view, which @p keep keeps alive.
    static Payload borrowed(dstd::Span<const std::byte> view,
                            dstd::SharedPtr<const void> keep) noexcept {
        Payload payload;
        payload.view_ = view;
        payload.keep_ = std::move(keep);
        return payload;
    }

    dstd::Span<const std::byte> bytes() const noexcept {
        return is_owned_
                   ? dstd::Span<const std::byte>(owned_.data(), owned_.size())
                   : view_;
    }

    std::size_t size() const noexcept { return bytes().size(); }

   private:
    dstd::Vector<std::byte> owned_;
    dstd::Span<const std::byte> view_;
    dstd::SharedPtr<const void> keep_;
    bool is_owned_ = false;
};

/// A parked receive into a caller buffer that a provider handed out, so the
/// bytes of its message land in the buffer straight from the wire.
struct Landing {
    dstd::Span<std::byte> buffer;
    /// Ends the receive with the bytes that landed, after they did.
    dstd::Function<void(std::size_t bytes)> done;
};

/**
 * @brief Ordered point-to-point bytes between two ranks of a group.
 *
 * A send takes the bytes and parks while its peer has no receive waiting for
 * them, holding at most the depth the caller names. A receive takes the first
 * message of its @ref MessageKey that is not taken yet. Both end exactly once,
 * with no error or with the error that ended them; an error pointer is valid
 * during the call only. A provider that runs its own progress thread completes
 * on an executor of the program, never on that thread. The bytes of a message
 * belong to the message once it is sent, or stay alive under its keep-alive,
 * so a send takes the @ref Payload and a receive gives a view of its bytes for
 * the call only.
 *
 * A call that parks gives its handle, so its owner cancels that call alone
 * without ending the group of the others. The provider decides the outcome of a
 * cancel under its own lock: a call it still holds ends with the error of the
 * cancel, and any other call keeps the outcome it has.
 */
class Messaging {
   public:
    /// Ends a send or a receive: @p why is null when it ended well.
    using SendDone = dstd::Function<void(const dstd::Error* why)>;
    /// Ends a receive with the bytes of its message, valid during the call
    /// only.
    using RecvDone = dstd::Function<void(dstd::Span<const std::byte> bytes,
                                         const dstd::Error* why)>;
    /// Ends a receive of any source with the rank that sent the bytes.
    using RecvFromDone = dstd::Function<void(std::uint32_t source,
                                             dstd::Span<const std::byte> bytes,
                                             const dstd::Error* why)>;
    /// Ends a receive into a caller buffer with the bytes it took.
    using RecvIntoDone = dstd::Function<void(
        std::uint32_t source, std::size_t bytes, const dstd::Error* why)>;

    Messaging() = default;
    Messaging(const Messaging&) = delete;
    Messaging& operator=(const Messaging&) = delete;
    virtual ~Messaging() = default;

    /// Sends @p payload on the stream of @p key, which holds at most
    /// @p opts.depth messages for a receive and does what @p opts.overflow
    /// names when it does; a receive already parked on that stream takes them
    /// here.
    virtual OpId send(const MessageKey& key, Payload payload, StreamOpts opts,
                      SendDone done) = 0;

    /// Receives the first message of @p key that no earlier receive took.
    virtual OpId recv(const MessageKey& key, RecvDone done) = 0;

    /// Receives the first message that any source sent to @p key.destination on
    /// its tag, when no earlier receive took it. Sources take their turn: the
    /// receive serves the source after the one the last receive served. It
    /// fails with @ref ChannelError::Closed once @p senders sources closed
    /// their stream and none holds a message.
    virtual OpId recv_any_source(const MessageKey& key, std::uint32_t senders,
                                 RecvFromDone done) = 0;

    /// Receives the first message of @p key, or of any source when @ref source
    /// is @ref ANY_SOURCE, into @p buffer, when it fits: the completion names
    /// the bytes it took. A longer message fails with @ref ChannelError::TooBig
    /// and stays first in its stream, so a later receive takes it again.
    /// @p senders is as for @ref recv_any_source.
    virtual OpId recv_into(const MessageKey& key, std::uint32_t senders,
                           dstd::Span<std::byte> buffer, RecvIntoDone done) = 0;

    /// Takes the receive into a buffer that the next message of @p key of
    /// @p bytes bytes would pair with, when that is the first receive of its
    /// stream, no earlier message waits, no receive of any source would take
    /// it, and its buffer holds @p bytes. The caller then writes the bytes into
    /// the buffer and ends the receive with @ref Landing::done; the receive can
    /// no longer be cancelled. Gives nothing when any of that fails, and then
    /// the message goes through @ref send as any other. A provider that never
    /// receives into a caller's buffer straight from a wire gives nothing.
    virtual dstd::Optional<Landing> claim_landing(const MessageKey& key,
                                                  std::size_t bytes) {
        (void)key;
        (void)bytes;
        return {};
    }

    /// A test hook: how many bytes this provider received straight into the
    /// buffer of a receive, with no copy through a message of its own.
    virtual std::uint64_t landed_bytes() const noexcept { return 0; }

    /// Ends the call @p op of @p key with @p why when the provider still holds
    /// it, and gives whether it did. A receive it held keeps its message for a
    /// later receive. A call it no longer holds, a send its stream stored and a
    /// call it already paired are not cancelled: they keep their outcome, which
    /// reaches the call through its completion. A provider whose peer is
    /// another process decides at that peer: it gives true while it asks the
    /// peer, and the call ends with @p why only when the peer still held the
    /// message, and from the peer's own result otherwise.
    virtual bool cancel(const MessageKey& key, OpId op, dstd::Error why) = 0;

    /// Sends the close of the stream of @p key behind every message its sender
    /// sent on it, also when it sent none: a receive takes what came before the
    /// close and then fails with @ref ChannelError::Closed. A send after a
    /// close fails with it too.
    virtual void close(const MessageKey& key) = 0;

    /// Fails every parked send and receive of group @p group with @p why, and
    /// every later one of that group at once, so a caller that gave up on a
    /// group holds nothing of it. The first abort of a group keeps its error.
    virtual void abort(std::uint64_t group, dstd::Error why) = 0;

    /// Told by a provider that has other processes, when a peer announced that
    /// it aborted @p group: the group of this process aborts with the same
    /// error. @p parent is the group the aborted one was made from by a split,
    /// or 0. Called on a thread of the provider, never under a lock of it.
    using PeerAbort = dstd::Function<void(
        std::uint64_t group, std::uint64_t parent, const dstd::Error& why)>;

    /// Sets what a peer's abort calls. A provider with no other process never
    /// calls it.
    virtual void on_peer_abort(PeerAbort handler) { (void)handler; }

    /// Tells every other process that @p group, made from @p parent by a split
    /// or 0, aborted with @p why, so their
    /// calls on it end with the same error. Called once per group by the
    /// first abort, after @ref abort; a provider with no other process does
    /// nothing.
    virtual void announce_abort(std::uint64_t group, std::uint64_t parent,
                                const dstd::Error& why) {
        (void)group;
        (void)parent;
        (void)why;
    }

    /// Tells the provider the members of @p group as world ranks in group
    /// order, when this process makes the group, so a provider that has other
    /// processes sends to the process of a group rank, and tells only the
    /// members of an abort. A group made by a split is made by the members of
    /// its parent only. A group never bound names its ranks as world ranks.
    virtual void bind_group(std::uint64_t group,
                            dstd::Span<const std::uint32_t> members) {
        (void)group;
        (void)members;
    }

    /// Forgets a group whose last handle is gone: fails the messages of @p
    /// group that are still parked with @ref dstd::Condition::Cancelled, erases
    /// its streams and forgets what @ref abort said of it. Nothing of the group
    /// sends or receives after this.
    virtual void release(std::uint64_t group) = 0;
};

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_MESSAGING_H
