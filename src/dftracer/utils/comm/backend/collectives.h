// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_COLLECTIVES_H
#define DFTRACER_UTILS_COMM_BACKEND_COLLECTIVES_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dftracer::utils::comm::backend {

/// The count a member that does not check its element count names.
inline constexpr std::uint64_t UNCHECKED_COUNT = ~std::uint64_t{0};

/// Bits of @ref Record::flags.
inline constexpr std::uint32_t RECORD_TABLE = 1u;
inline constexpr std::uint32_t RECORD_WANTS = 2u;
inline constexpr std::uint32_t RECORD_COMBINE = 4u;
inline constexpr std::uint32_t RECORD_PERSONAL = 8u;

/**
 * @brief The fixed record one member brings to one slot of a group.
 *
 * @ref kind is the call the member makes: two members of one slot must name the
 * same one, and a provider that sees two different ones ends the slot early.
 * The caller gives the other fields their meaning; a provider reads only
 * @ref kind and @ref flags. A call with @ref RECORD_TABLE brings a
 * @ref Contribution that goes into the table of the slot, @ref RECORD_WANTS
 * says that this member takes the table, and @ref RECORD_COMBINE asks the
 * provider to fold the contributions in arrival order. @ref RECORD_PERSONAL
 * says that the contribution holds one piece per member, which the provider
 * splits: see @ref Contribution.
 */
struct Record {
    std::uint32_t kind = 0;
    std::uint32_t version = 0;
    std::uint32_t color = 0;
    std::uint32_t key = 0;
    std::uint32_t root = 0;
    std::uint32_t elem_size = 0;
    std::uint64_t count = 0;
    std::uint32_t flags = 0;
    std::uint32_t reduce = 0;
};

static_assert(sizeof(Record) == 40, "the wire record has a fixed size");

/// Folds @p bytes bytes of @p in into @p acc.
using Combine = dstd::Function<void(std::byte* acc, const std::byte* in,
                                    std::size_t bytes)>;

/**
 * @brief The bytes one member brings to a slot with a @ref RECORD_TABLE record.
 *
 * @ref keep owns the block that @ref bytes views, so the provider reads it
 * until it ended the slot, also when the call that made it already failed.
 */
struct Contribution {
    dstd::Span<const std::byte> bytes;
    dstd::SharedPtr<const void> keep;
    /// The fold of a @ref RECORD_COMBINE call. The provider runs it on the
    /// thread that folds, outside its own lock, once per contribution and never
    /// at the same time for one slot. After the provider dropped, released or
    /// ended the slot it starts no further merge, except at most one that had
    /// already passed its check and may still run once after that call
    /// returns; the memory it reads stays valid until it returns. It may call
    /// the provider, but not the group of its own slot.
    Combine combine;
};

/**
 * The bytes of a contribution of a @ref RECORD_PERSONAL record: `members`
 * 64-bit sizes, then the pieces, piece `d` for member `d`. The sizes must add
 * up to the rest of the bytes, which a provider checks before it splits them.
 */
inline bool personal_fits(dstd::Span<const std::byte> bytes,
                          std::uint32_t members) noexcept {
    const std::uint64_t head = std::uint64_t{members} * sizeof(std::uint64_t);
    if (bytes.size() < head) return false;
    std::uint64_t total = 0;
    for (std::uint32_t at = 0; at < members; ++at) {
        std::uint64_t size = 0;
        std::memcpy(&size, bytes.data() + at * sizeof(std::uint64_t),
                    sizeof(size));
        if (size > bytes.size() - head - total) return false;
        total += size;
    }
    return total == bytes.size() - head;
}

/// The size of piece @p dest of a contribution that @ref personal_fits.
inline std::uint64_t personal_size(dstd::Span<const std::byte> bytes,
                                   std::uint32_t dest) noexcept {
    std::uint64_t size = 0;
    std::memcpy(&size, bytes.data() + dest * sizeof(std::uint64_t),
                sizeof(size));
    return size;
}

/**
 * @brief The contributions of every member in one owned buffer, as a slot ends.
 *
 * The bytes of member `r` are `data[offsets[r], offsets[r + 1])`, with 64-bit
 * offsets. @ref keep owns what @ref data and @ref offsets view, so a copy of
 * the keep-alive lets a view outlive the slot. A provider that folded the
 * contributions gives the fold as entry 0. @ref offsets is empty when the slot
 * carried no table.
 */
struct Table {
    dstd::Span<const std::byte> data;
    dstd::Span<const std::uint64_t> offsets;
    dstd::SharedPtr<const void> keep;

    /// How many entries the table has.
    std::uint32_t size() const noexcept {
        return offsets.empty() ? 0
                               : static_cast<std::uint32_t>(offsets.size() - 1);
    }

    /// The bytes of entry @p index.
    dstd::Span<const std::byte> at(std::uint32_t index) const noexcept {
        return data.subspan(offsets[index],
                            offsets[index + 1] - offsets[index]);
    }
};

/// The memory a provider's @ref Table views.
struct TableBuffer {
    dstd::Vector<std::byte> data;
    dstd::Vector<std::uint64_t> offsets;
};

/// The table over @p buffer, which it keeps alive.
inline Table make_table(dstd::SharedPtr<const TableBuffer> buffer) {
    Table table;
    table.data =
        dstd::Span<const std::byte>(buffer->data.data(), buffer->data.size());
    table.offsets = dstd::Span<const std::uint64_t>(buffer->offsets.data(),
                                                    buffer->offsets.size());
    table.keep = std::move(buffer);
    return table;
}

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
    /// What the members brought, when the records carry @ref RECORD_TABLE.
    Table table;
    /// For records with @ref RECORD_PERSONAL, the table of each member, by
    /// group rank: entry `s` of the table of member `d` is piece `d` of the
    /// contribution of member `s`. A member this provider does not serve has an
    /// empty table: a provider that sees every member serves them all, and one
    /// that collects at another process serves its own rank. @ref table is
    /// empty. Empty for any other slot.
    dstd::Span<const Table> tables;
};

/// One byte per member, 1 for a member a provider saw arrive. Empty means the
/// provider cannot see every member of the slot.
using Arrival = dstd::Vector<std::uint8_t>;

/**
 * @brief Collects one @ref Record, and the @ref Contribution of a call that
 * carries a table, from every member of a group at one slot of that group.
 *
 * A slot is a group id and a sequence number of a member, so the calls of one
 * group match their peers by both. A provider reports arrival and moves
 * the bytes of the contributions into one @ref Table: the handshake, the error
 * text, the abort, the merge of a reduction in rank order and the deadlines
 * belong to the caller.
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

    /// True when every member of every slot lives in this process, so a call
    /// may hand an object to the other members without serializing it.
    virtual bool shares_memory() const noexcept { return false; }

    /**
     * @brief Arrives at slot @p seq of group @p group_id with @p record and
     * @p contribution, for @p rank of @p members, and gives true while the slot
     * waits for members.
     *
     * The provider keeps the @p done of the first arrival at a slot and calls
     * it once. It calls it before the last member arrives when two members of
     * the slot name different @ref Record::kind values, which a provider that
     * sees every member reports as soon as it sees them.
     */
    virtual bool arrive(std::uint64_t group_id, std::uint64_t seq,
                        std::uint32_t members, std::uint32_t rank,
                        const Record& record, Contribution&& contribution,
                        Done done) = 0;

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
