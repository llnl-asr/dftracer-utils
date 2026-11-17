// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_BULK_H
#define DFTRACER_UTILS_COMM_BACKEND_BULK_H

#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/bulk.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::comm::backend {

/**
 * @brief Memory a rank exposes, and the copies between it and the memory of a
 * peer.
 *
 * The ranks of a group name exposures by @ref BufferRef. A provider moves the
 * bytes a way its transport allows: the local provider copies between ranks of
 * one process, and a provider over a @ref Messaging runs a protocol that the
 * receiving end drives. The checks that every provider shares, the group of a
 * ref and the range, are made above the seam by @ref check_transfer; the
 * provider answers for what only it knows: whether the exposure exists and is
 * still open.
 *
 * Every call ends exactly once through its @ref Done, which never runs under a
 * lock of the provider, and a provider that runs a progress thread of its own
 * completes on an executor of the program, never on that thread. A transfer
 * holds the exposure it reads or writes: the memory stays valid for the owner
 * until @ref unexpose reports the exposure drained.
 */
class Bulk {
   public:
    /// Ends a pull or a push with the bytes it moved, or with the error that
    /// ended it; the error pointer is valid during the call only.
    using Done =
        dstd::Function<void(std::uint64_t bytes, const dstd::Error* why)>;
    /// Runs once an exposure has no transfer left.
    using Drained = dstd::Function<void()>;

    Bulk() = default;
    Bulk(const Bulk&) = delete;
    Bulk& operator=(const Bulk&) = delete;
    virtual ~Bulk() = default;

    /// Exposes @p memory as rank @p rank of group @p group and gives its ref.
    /// The memory must stay valid until @ref unexpose drained.
    virtual dstd::Result<BufferRef> expose(std::uint64_t group,
                                           std::uint32_t rank,
                                           dstd::Span<std::byte> memory,
                                           Access access, MemoryKind kind) = 0;

    /// Closes the exposure of @p ref to new transfers at once, and runs
    /// @p done when no transfer on it is left. A ref that names no open
    /// exposure runs it at once.
    virtual void unexpose(const BufferRef& ref, Drained done) = 0;

    /// Copies the exposure from @p offset to its end into @p dst, as rank
    /// @p rank, which the caller checked fits. @p done gets the bytes copied.
    /// The call gives the handle that @ref cancel takes, or @ref NO_OP when it
    /// ended at once.
    virtual OpId pull(std::uint32_t rank, const BufferRef& ref,
                      std::uint64_t offset, dstd::Span<std::byte> dst,
                      Done done) = 0;

    /// Copies @p src into the exposure from @p offset, as rank @p rank, which
    /// the caller checked fits. @p src stays valid until @p done runs.
    virtual OpId push(std::uint32_t rank, const BufferRef& ref,
                      std::uint64_t offset, dstd::Span<const std::byte> src,
                      Done done) = 0;

    /// Ends the pull or push @p op of group @p group with @p why when the
    /// provider still holds it, and gives whether it did. A transfer that no
    /// longer waits keeps its outcome. A provider that moves bytes through a
    /// transport ends the call only once nothing of the caller's memory is in
    /// flight, so @p done may run after this returns.
    virtual bool cancel(std::uint64_t group, OpId op, dstd::Error why) = 0;

    /// Fails every pull and push of group @p group with @p why, and every later
    /// one at once. The first abort of a group keeps its error.
    virtual void abort(std::uint64_t group, dstd::Error why) = 0;

    /// Tells the provider the members of @p group as world ranks in group
    /// order, as @ref Messaging::bind_group does.
    virtual void bind_group(std::uint64_t group,
                            dstd::Span<const std::uint32_t> members) {
        (void)group;
        (void)members;
    }

    /// Forgets a group whose last handle is gone: closes its exposures and
    /// forgets what @ref abort said of it.
    virtual void release(std::uint64_t group) = 0;

    /// A test hook: caps the bytes of one range at @p bytes, so a transfer
    /// runs in many ranges on small data. Does nothing by default.
    virtual void limit_range_bytes(std::uint64_t bytes) noexcept {
        (void)bytes;
    }

    /// A test hook: gives each peer @p bytes of credit in place of the share
    /// of the memory budget. Does nothing by default.
    virtual void limit_credit_bytes(std::uint64_t bytes) noexcept {
        (void)bytes;
    }

    /// A test hook: the most bytes any one peer had in flight toward its
    /// receiver at once, since the start of the provider.
    virtual std::uint64_t peak_in_flight() const noexcept { return 0; }

    /// A test hook: the bytes that transfers hold of the credit of every peer
    /// now; 0 when none holds any.
    virtual std::uint64_t credit_in_flight() const noexcept { return 0; }

    /// A test hook: takes @p bytes of the credit that @p receiver has toward
    /// @p peer in group @p group, and holds them until the token goes, so the
    /// transfers of that pair wait. Gives an empty token by default.
    virtual dstd::SharedPtr<void> hold_credit(std::uint64_t group,
                                              std::uint32_t receiver,
                                              std::uint32_t peer,
                                              std::uint64_t bytes) {
        (void)group;
        (void)receiver;
        (void)peer;
        (void)bytes;
        return {};
    }
};

/// The bulk provider of a run whose ranks are all in this process: a transfer
/// copies between the two memories, in ranges that its credit bounds, and a
/// ref reads no wire.
dstd::SharedPtr<Bulk> make_local_bulk();

/// The bulk provider over @p messaging, for ranks in other processes: the
/// receiving end of a transfer asks for ranges and grants credit, and the
/// owner moves bytes only for a range that was asked for. Works on any
/// @ref Messaging, also the local one, which a test uses.
dstd::SharedPtr<Bulk> make_message_bulk(dstd::SharedPtr<Messaging> messaging);

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_BULK_H
