// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_GROUP_H
#define DFTRACER_UTILS_COMM_GROUP_H

#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::comm {

namespace backend {
class Collectives;
class Messaging;
}  // namespace backend

namespace detail {
struct Backend;
struct GroupState;
struct Hooks;
}  // namespace detail

/// What one call may add to the defaults of its group.
struct CallOpts {
    /// A deadline on the steady clock, in nanoseconds since its epoch, or 0 for
    /// the default timeout of the group, counted from the moment of this call.
    /// A deadline that passes on one rank aborts the group.
    std::uint64_t deadline_ns = 0;
    /// A token already stopped when the call is made cancels the call, which
    /// aborts the group. A stop that arrives while the call is in flight is not
    /// observed; use @ref Op::cancel or a deadline for that.
    coro::StopToken stop{};
};

/**
 * @brief One set of ranks that make the same collectives in the same order.
 *
 * A handle: copies name the same membership and the same call stream. The
 * collectives of one rank match in the order they are called, so every member
 * calls them in the same order; a rank that calls another collective than its
 * peers is an error, never a wait. A group with no members is empty
 * (@ref size is 0, @ref rank and @ref id are 0) and its collectives end at once
 * with a value.
 *
 * Every call of a member belongs to the group and to a sequence number of that
 * member, and peers match on both. Aborting a group, canceling a call and a
 * passed deadline each fail every pending and later call of the group on every
 * rank, with one error that names the rank that caused it.
 */
class Group {
   public:
    /// The color a rank passes to @ref split to stay out of every new group.
    static constexpr std::uint32_t NO_COLOR = 0xFFFFFFFFu;

    /// An empty group.
    Group() noexcept = default;

    /// Made by @ref Communicator and @ref split; a user never makes one.
    Group(dstd::SharedPtr<detail::Backend> backend,
          dstd::SharedPtr<detail::GroupState> state,
          std::uint32_t rank) noexcept
        : backend_(std::move(backend)), state_(std::move(state)), rank_(rank) {}

    /// This rank's number in the group, from 0 to @ref size minus 1.
    std::uint32_t rank() const noexcept { return rank_; }

    /// How many ranks the group has.
    std::uint32_t size() const noexcept;

    /// The id the group was made with, unique among the groups of one
    /// @ref Communicator.
    std::uint64_t id() const noexcept;

    /**
     * @brief Collective: each member picks a color and a key, and the members
     * of one color form a new group.
     *
     * The new ranks follow @p key and, for equal keys, the rank in this group.
     * A member that passes @ref NO_COLOR gets an empty group. Every member must
     * call it, and every member also gets a group of its own: a rank that no
     * longer participates stays out of later calls of the new group.
     */
    Op<Group> split(std::uint32_t color, std::uint32_t key, CallOpts opts = {});

    /// Collective: ends once every member called it.
    Op<void> barrier(CallOpts opts = {});

    /// @ref barrier whose error names the members that did not arrive when its
    /// deadline passes.
    Op<void> monitored_barrier(CallOpts opts = {});

    /// The timeout the later calls of this rank use when @ref CallOpts names
    /// no deadline, counted from the moment each call is made. 0, the default,
    /// sets none.
    void set_default_deadline(std::uint64_t timeout_ns) noexcept;

    /**
     * @brief Fails every pending and later call of the group, on every rank,
     * with one error that names @p why and this rank.
     *
     * The first abort of a group wins and a later one is dropped.
     */
    void abort(dstd::Error why) noexcept;

   private:
    dstd::SharedPtr<detail::Backend> backend_;
    dstd::SharedPtr<detail::GroupState> state_;
    std::uint32_t rank_ = 0;
};

/**
 * @brief The members of one run. Makes the groups.
 *
 * The local-threads backend runs every rank as a coroutine in this process, on
 * the executor given at start, so a program written against it runs unchanged
 * on a backend that spreads the ranks over nodes. Every rank of the run is
 * reachable from every thread of the process.
 */
class Communicator {
   public:
    /// Starts the local-threads backend with @p ranks ranks on @p executor,
    /// which must outlive the communicator and every group it made. Binds the
    /// bootstrap, the messaging and the collectives of the local backend.
    static dstd::Result<Communicator> start_local(std::uint32_t ranks,
                                                  coro::Executor& executor);

    /// Made by @ref start_local.
    explicit Communicator(dstd::SharedPtr<detail::Backend> backend) noexcept
        : backend_(std::move(backend)) {}

    /// The group of every rank of the run. @p rank must be below its size.
    Group world(std::uint32_t rank) noexcept;

    /// The ranks that share this node, which is every rank for this backend.
    Group local(std::uint32_t rank) noexcept;

    /// How many ranks the run has.
    std::uint32_t size() const noexcept;

   private:
    friend struct detail::Hooks;
    dstd::SharedPtr<detail::Backend> backend_;
    dstd::SharedPtr<detail::GroupState> world_;
    /// One group per node of the run, by world rank.
    dstd::Vector<Group> node_;
};

namespace detail {

/// Test hooks that force the states a correct peer never reaches.
struct Hooks {
    /// Makes @p rank report @p version at the protocol handshake.
    static void set_protocol_version(Communicator& comm, std::uint32_t rank,
                                     std::uint32_t version) noexcept;

    /// Calls that are alive now, of every communicator.
    static std::size_t live_ops() noexcept;

    /// Groups that are alive now, of every communicator.
    static std::size_t live_groups() noexcept;

    /// Holds the timer thread in @ref expire right before it takes the group
    /// lock, which is where a completing call can land. Null clears it.
    static void set_expire_gate(void (*gate)()) noexcept;

    /// Clears what @ref expire reports to a test.
    static void reset_expire_signal() noexcept;

    /// True once @ref expire reached the group lock, and once it returned. A
    /// test waits on these instead of on time.
    static bool expire_at_lock() noexcept;
    static bool expire_returned() noexcept;

    /// The provider that moves the messages of @p comm.
    static dstd::SharedPtr<backend::Messaging> messaging(
        const Communicator& comm) noexcept;

    /// Collects the calls of @p comm with @p collectives from now on. A test
    /// binds another one before it makes the first call.
    static void set_collectives(
        Communicator& comm,
        dstd::SharedPtr<backend::Collectives> collectives) noexcept;

    /// Releases the groups of @p comm on their provider, as the end of a
    /// communicator does, so a test can ask what a dead group left behind.
    static void release_groups(Communicator& comm) noexcept;
};

}  // namespace detail

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_GROUP_H
