// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_COLLECTIVES_H
#define DFTRACER_UTILS_COMM_BACKEND_COLLECTIVES_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::comm::backend {

/**
 * @brief The fixed record one member brings to one slot of a group.
 *
 * @ref kind is the call the member makes: two members of one slot must name the
 * same one, and a provider that sees two different ones ends the slot early.
 * The caller gives @ref version, @ref color and @ref key their meaning.
 */
struct Record {
    std::uint32_t kind = 0;
    std::uint32_t version = 0;
    std::uint32_t color = 0;
    std::uint32_t key = 0;
};

/**
 * @brief How one slot ended, as its provider reports it.
 */
struct SlotEnd {
    /// One record per member, in group rank order; a record means what
    /// @ref seen says of its member.
    dstd::Span<const Record> records;
    /// One byte per member, 1 for a member that arrived, when the slot ended
    /// before every member did; empty when every member arrived.
    dstd::Span<const std::uint8_t> seen;
    /// The group rank that ended the slot: the one whose record or message
    /// broke it, or the last one to arrive.
    std::uint32_t rank = 0;
    /// Null when the slot ended with the record of every member; the error that
    /// ended it otherwise.
    const dstd::Error* why = nullptr;
};

/// One byte per member, 1 for a member a provider saw arrive. Empty means the
/// provider cannot see every member of the slot.
using Arrival = dstd::Vector<std::uint8_t>;

/**
 * @brief Collects one @ref Record from every member of a group at one slot of
 * that group.
 *
 * A slot is a group id and a sequence number of a member, so the calls of one
 * group match their peers by both. A provider reports arrival only: the
 * handshake, the error text, the abort and the deadlines belong to the caller.
 * It never runs a @ref Done under its own lock, so neither the caller's group
 * lock nor a provider lock is held when one runs.
 */
class Collectives {
   public:
    /// Ends a slot once, with how it ended, on the thread that ended it.
    using Done = dstd::Function<void(const SlotEnd& end)>;

    Collectives() = default;
    Collectives(const Collectives&) = delete;
    Collectives& operator=(const Collectives&) = delete;
    virtual ~Collectives() = default;

    /**
     * @brief Arrives at slot @p seq of group @p group_id with @p record, for
     * @p rank of @p members, and gives true while the slot waits for members.
     *
     * The provider keeps the @p done of the first arrival at a slot and calls
     * it once. It calls it before the last member arrives when two members of
     * the slot name different @ref Record::kind values, which a provider that
     * sees every member reports as soon as it sees them.
     */
    virtual bool arrive(std::uint64_t group_id, std::uint64_t seq,
                        std::uint32_t members, std::uint32_t rank,
                        const Record& record, Done done) = 0;

    /**
     * @brief Ends slot @p seq of @p group_id without its completion and frees
     * what it holds. Its @ref Done never runs.
     *
     * @p arrived gets one byte per member, 1 for a member the slot saw arrive,
     * and stays empty when this provider cannot see every member of the slot: a
     * rank that collects through another one reports nothing rather than a
     * guess.
     */
    virtual void drop(std::uint64_t group_id, std::uint64_t seq,
                      Arrival* arrived) = 0;

    /**
     * @brief Frees what the provider holds for @p group_id, whose last handle
     * is gone, so no message of it stays parked.
     *
     * The group never makes another call, so nothing arrives for it after this.
     */
    virtual void release(std::uint64_t group_id) = 0;
};

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_COLLECTIVES_H
