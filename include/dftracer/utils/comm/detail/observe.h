// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_DETAIL_OBSERVE_H
#define DFTRACER_UTILS_COMM_DETAIL_OBSERVE_H

#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/stats.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::comm::detail {

/// How a call ended, for the counters and the trace.
enum class EndKind : std::uint8_t { Ok, Failed, Cancelled };

inline constexpr std::size_t CALL_KIND_COUNT =
    static_cast<std::size_t>(CallKind::Push) + 1;

/// What the calls of one kind ended with. Relaxed: they are read for a
/// snapshot, never to decide anything.
struct EndCounters {
    std::atomic<std::uint64_t> ok{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> cancelled{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_received{0};
};

/**
 * @brief The counters of every kind of call of one group.
 *
 * The calls of a collective kind are counted under the group mutex, which every
 * one of them takes anyway, so a counter there is a load and a store and not an
 * atomic read-modify-write; the other kinds count with one relaxed add each. A
 * kind is counted by one of the two ways only, so they never mix on a counter.
 * The starts and the ends sit in lines of their own, so the ranks of a group
 * that run at once do not pass a line between them.
 */
struct KindTable {
    std::array<std::atomic<std::uint64_t>, CALL_KIND_COUNT> started{};
    char gap[64] = {};
    std::array<EndCounters, CALL_KIND_COUNT> ended{};
    char tail[64] = {};
};

/**
 * @brief One DFTracer event file per rank, in a directory, with one event per
 * call that ended.
 *
 * An event is buffered under one mutex and appended to its file when the buffer
 * is large and when the tracer goes; nothing is written per byte.
 */
class Tracer {
   public:
    /// Writes into @p directory, which is made when missing. Gives nothing when
    /// it cannot be, after one warning that names it. Each tracer of a process
    /// numbers its files, so two communicators never write the same one.
    static dstd::SharedPtr<Tracer> make(dstd::StringView directory);

    Tracer(dstd::String directory, std::uint64_t number);
    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;
    ~Tracer();

    /// Adds the event of a call of world rank @p world_rank that ended now
    /// and lasted @p dur_ns.
    void event(std::uint32_t world_rank, CallKind kind, std::uint64_t group,
               std::uint64_t seq, std::uint64_t dur_ns, std::uint64_t bytes,
               EndKind end);

    /// Appends every buffered event to its file.
    void flush();

   private:
    struct Sink {
        dstd::String path;
        dstd::String buffer;
        bool started = false;
    };

    void write_locked(Sink& sink);
    void warn_once(const dstd::String& path, const dstd::Error& why);

    dstd::String directory_;
    std::uint64_t pid_ = 0;
    std::uint64_t number_ = 0;
    bool warned_ = false;
    std::mutex mutex_;
    std::uint64_t next_id_ = 0;
    dstd::Map<std::uint32_t, Sink> sinks_;
};

/// What a communicator observes: the counters of the groups that are gone and
/// the trace, if it is on.
struct Observer {
    explicit Observer(dstd::SharedPtr<Tracer> trace) noexcept
        : tracer(std::move(trace)) {}

    dstd::SharedPtr<Tracer> tracer;
    KindTable retired;
};

/// The counters of one group, shared by the group and by the calls it made. The
/// counts of a group that is gone join those of @ref Observer::retired.
struct Tally {
    Tally(dstd::SharedPtr<Observer> owner, std::uint64_t group_id) noexcept
        : observer(std::move(owner)),
          group(group_id),
          traced(static_cast<bool>(observer->tracer)) {}
    Tally(const Tally&) = delete;
    Tally& operator=(const Tally&) = delete;
    ~Tally();

    dstd::SharedPtr<Observer> observer;
    std::uint64_t group = 0;
    /// Whether the communicator traces, kept here so a call reads one byte.
    bool traced = false;
    KindTable table;
};

/// What one call knows of itself, for the counters, the trace and the pending
/// list. Set before the call is shared and never changed, except the start.
///
/// The tally is not owned: it outlives every use the call makes of it. A
/// collective call is kept by its group, which owns the tally, until the call
/// ends or the group is gone; a channel call holds its channel and a bulk call
/// holds the tally itself.
struct CallMeta {
    Tally* tally = nullptr;
    std::uint64_t seq = 0;
    /// On the steady clock, and read only when it is needed: when the trace is
    /// on, which needs the duration, and when the call has a deadline. Zero
    /// otherwise, and a call of a slot is then as old as its slot.
    std::uint64_t start_ns = 0;
    std::uint32_t world_rank = 0;
    CallKind kind = CallKind::Barrier;
    /// The group counts the good end of this call together with the others of
    /// its slot, so the call does not.
    bool batched = false;
};

/// The meta of a call of @p kind that the rank of world rank @p world_rank
/// makes, not counted yet. @p tally may be null: the call is then not observed.
/// @p batched is true for a call of a slot, whose good end the group counts
/// with @ref count_batch_locked or @ref count_batch.
CallMeta make_meta(Tally* tally, CallKind kind, std::uint32_t world_rank,
                   bool batched = false) noexcept;

/// Counts the start of a call of a collective kind, with the group mutex held.
void count_start_locked(Tally& tally, CallKind kind) noexcept;

/// The meta of a call that no group mutex covers, counted as started.
CallMeta begin_call(Tally* tally, CallKind kind, std::uint32_t world_rank);

/// How long ago @p meta began, in nanoseconds. A call that read no clock is as
/// old as its slot, which began at @p slot_start_ns on the coarse clock.
std::uint64_t call_age_ns(const CallMeta& meta,
                          std::uint64_t slot_start_ns = 0) noexcept;

/// Counts the end of a call and, when the trace is on, writes its event. It
/// runs before the call is marked done, so whoever waits for the call sees it
/// counted.
void end_call(const CallMeta& meta, EndKind end, std::uint64_t bytes_sent,
              std::uint64_t bytes_received) noexcept;

/// Counts @p ok calls of @p kind that end well together, with the bytes they
/// moved, with the group mutex held and before they end.
void count_batch_locked(Tally& tally, CallKind kind, std::uint64_t ok,
                        std::uint64_t bytes_sent,
                        std::uint64_t bytes_received) noexcept;

/// The same without the group mutex.
void count_batch(Tally& tally, CallKind kind, std::uint64_t ok,
                 std::uint64_t bytes_sent,
                 std::uint64_t bytes_received) noexcept;

/// Adds the counters of @p tally to @p rows, one row per kind that was used.
void add_stats(const Tally& tally, dstd::Vector<CallStats>& rows);

/// Adds the retired counters of @p observer as the row of group 0.
void add_retired(const Observer& observer, dstd::Vector<CallStats>& rows);

}  // namespace dftracer::utils::comm::detail

#endif  // DFTRACER_UTILS_COMM_DETAIL_OBSERVE_H
