// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_MESSAGING_H
#define DFTRACER_UTILS_COMM_BACKEND_MESSAGING_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::comm::backend {

/// One stream of messages: the messages two ranks of a group send each other on
/// one tag, in the order their sender sent them.
///
/// The tag of a stream a collective uses holds its sequence number, so a
/// message of one call never answers the receive of another.
struct MessageKey {
    std::uint64_t group = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint64_t tag = 0;

    friend bool operator<(const MessageKey& a, const MessageKey& b) noexcept {
        if (a.group != b.group) return a.group < b.group;
        if (a.source != b.source) return a.source < b.source;
        if (a.destination != b.destination)
            return a.destination < b.destination;
        return a.tag < b.tag;
    }
};

/**
 * @brief Ordered point-to-point bytes between two ranks of a group.
 *
 * A send takes the bytes and parks while its peer has no receive waiting for
 * them. A receive takes the first message of its @ref MessageKey that is not
 * taken yet. Both end exactly once, with no error or with the error that ended
 * them; an error pointer is valid during the call only. A provider that runs
 * its own progress thread completes on an executor of the program, never on
 * that thread. The bytes of a message belong to the message once it is sent, so
 * a send takes them and a receive gives a view of them for the call only.
 */
class Messaging {
   public:
    /// Ends a send or a receive: @p why is null when it ended well.
    using SendDone = dstd::Function<void(const dstd::Error* why)>;
    /// Ends a receive with the bytes of its message, valid during the call
    /// only.
    using RecvDone = dstd::Function<void(dstd::Span<const std::byte> bytes,
                                         const dstd::Error* why)>;

    Messaging() = default;
    Messaging(const Messaging&) = delete;
    Messaging& operator=(const Messaging&) = delete;
    virtual ~Messaging() = default;

    /// Sends @p bytes on the stream of @p key; a receive already parked on that
    /// stream takes them here.
    virtual void send(const MessageKey& key, dstd::Vector<std::byte> bytes,
                      SendDone done) = 0;

    /// Receives the first message of @p key that no earlier receive took.
    virtual void recv(const MessageKey& key, RecvDone done) = 0;

    /// Fails every parked send and receive of group @p group with @p why, and
    /// every later one of that group at once, so a caller that gave up on a
    /// group holds nothing of it.
    virtual void abort(std::uint64_t group, dstd::Error why) = 0;

    /// Forgets a group whose last handle is gone: fails the messages of @p
    /// group that are still parked with @ref dstd::Condition::Cancelled, erases
    /// its streams and forgets what @ref abort said of it. Nothing of the group
    /// sends or receives after this.
    virtual void release(std::uint64_t group) = 0;
};

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_MESSAGING_H
