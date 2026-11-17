// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_FAULT_MESSAGING_H
#define DFTRACER_UTILS_COMM_BACKEND_FAULT_MESSAGING_H

#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::coro {
class TimerService;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::comm::backend {

/// Which sends a rule touches. A field left at its default matches every send.
struct FaultMatch {
    /// The group rank of the sender, or @ref ANY_SOURCE.
    std::uint32_t source = ANY_SOURCE;
    /// The group rank of the receiver, or @ref ANY_SOURCE.
    std::uint32_t destination = ANY_SOURCE;
    /// What the stream carries.
    dstd::Optional<TagPurpose> purpose;
};

/// What happens to the sends a rule matches. The first rule that matches a send
/// decides it. The values every message gets are a function of the seed, the
/// stream and the place of the message in it, so the same seed gives the same
/// schedule whatever the threads do.
struct FaultRule {
    FaultMatch match;
    /// Extra latency of each message, drawn uniformly from this range of
    /// nanoseconds. Measured on the timers of the process, never by a sleep.
    std::uint64_t delay_min_ns = 0;
    std::uint64_t delay_max_ns = 0;
    /// A message is held until a stream holds this many, or until
    /// @ref FaultConfig::reorder_flush_ns passes; then every held message is
    /// released in a seeded order across streams. 0 holds nothing.
    std::uint32_t reorder_hold = 0;
    /// The chance that a message is lost, in parts per million. A lost message
    /// is never delivered, and its send ends as sent.
    std::uint32_t drop_per_million = 0;
    /// The most messages this rule loses from one stream (one key), or 0 for no
    /// limit.
    std::uint64_t drop_limit = 0;
};

/// A world rank that dies: from its @ref after_ops th send or receive on, every
/// send and receive of that rank fails.
struct FaultKill {
    std::uint32_t rank = 0;
    std::uint64_t after_ops = 0;
};

struct FaultConfig {
    std::uint64_t seed = 0;
    dstd::Vector<FaultRule> rules;
    dstd::Vector<FaultKill> kills;
    /// How long a held message waits for its stream to fill.
    std::uint64_t reorder_flush_ns = 1'000'000;
    /// Keeps @ref FaultMessaging::schedule; off, it stays empty.
    bool record_schedule = false;
};

enum class FaultAction : std::uint8_t {
    /// The message went on at once.
    Pass,
    /// The message waits for its delay.
    Delay,
    /// The message waits for its reorder window.
    Hold,
    /// The message is lost.
    Drop,
    /// The message left the provider for the one under it.
    Release,
};

/// One decision or release, in the order the provider made it.
struct FaultEvent {
    MessageKey key;
    /// The place of the message in its stream, from 0.
    std::uint64_t index = 0;
    FaultAction action = FaultAction::Pass;
    std::uint64_t delay_ns = 0;
};

/// What the provider did, in total.
struct FaultCounts {
    std::uint64_t passed = 0;
    std::uint64_t delayed = 0;
    std::uint64_t held = 0;
    std::uint64_t dropped = 0;
    /// Sends and receives that failed because their rank was killed.
    std::uint64_t killed_ops = 0;
};

/**
 * @brief A @ref Messaging that passes every call to another one and, by rule,
 * delays, reorders, drops or kills what goes through it.
 *
 * For tests and for chaos runs. One stream (group, sender, receiver, tag) stays
 * first in, first out whatever the rules do: a message never leaves before the
 * ones sent before it on its stream, so a reorder shows only between streams.
 * A delayed or held send is not parked in the provider below yet, so it has no
 * handle and cannot be cancelled until it is released.
 *
 * A kill makes every group that holds the rank abort with one error that names
 * it, as a peer process that announced its death does: the pending calls of
 * collectives, channels and bulk transfers end with it, on every rank. Groups
 * made after the kill are not aborted: the dead rank's own sends and receives
 * fail, and its peers wait for it until a deadline.
 *
 * A message that is lost is a lost message: the layers above end by a deadline
 * that names the ranks, never by a result that is silently wrong.
 */
class FaultMessaging : public Messaging {
   public:
    /// Kills world rank @p rank now.
    virtual void kill(std::uint32_t rank) = 0;

    /// Releases every held message now, in the seeded order.
    virtual void flush() = 0;

    virtual FaultCounts counts() const = 0;

    /// The decisions and releases so far; empty unless
    /// @ref FaultConfig::record_schedule.
    virtual dstd::Vector<FaultEvent> schedule() const = 0;
};

/// A fault provider over @p inner. @p timers runs the delays and the kill; it
/// must outlive the provider. With none, a delay or a hold ends at the next
/// call that reaches the provider.
dstd::SharedPtr<FaultMessaging> make_fault_messaging(
    dstd::SharedPtr<Messaging> inner, FaultConfig config,
    coro::TimerService* timers);

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_FAULT_MESSAGING_H
