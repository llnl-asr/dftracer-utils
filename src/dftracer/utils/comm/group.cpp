// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/detail/observe.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dftracer::utils::comm {

using dstd::Condition;
using dstd::Error;

namespace detail {

namespace {

/// Groups that are alive now, which the tests read to prove a finished call
/// pins nothing.
std::atomic<std::size_t> g_live_groups{0};

/// Set by a test to hold the timer thread before the group lock.
std::atomic<void (*)()> g_expire_gate{nullptr};
/// Reported by @ref expire to a test: it reached the group lock, and it
/// returned. A test waits on these rather than on time.
std::atomic<bool> g_expire_at_lock{false};
std::atomic<bool> g_expire_returned{false};

/// Reports the return of @ref expire on every path out of it.
struct ExpireSignal {
    ~ExpireSignal() {
        g_expire_returned.store(true, std::memory_order_release);
    }
};

/// The error of a call that a member or its waiter cancelled.
Error canceled_call() noexcept {
    return Error::from_format<"canceled a call">(Condition::Cancelled);
}

}  // namespace

std::size_t count_live_groups() noexcept {
    return g_live_groups.load(std::memory_order_acquire);
}

bool TimerTarget::take(dstd::SharedPtr<GroupState>& out_group,
                       dstd::SharedPtr<PendingOp>& out_op) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (!group) return false;
    out_group = group;
    out_op = op;
    return true;
}

bool TimerTarget::take_call(dstd::SharedPtr<PendingOp>& out_op) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (!op) return false;
    out_op = op;
    return true;
}

void TimerTarget::clear() noexcept {
    dstd::SharedPtr<GroupState> released_group;
    dstd::SharedPtr<PendingOp> released_op;
    {
        std::lock_guard<std::mutex> lock(mutex);
        released_group = std::move(group);
        released_op = std::move(op);
    }
    // Freed here, outside the lock.
}

GroupState::GroupState() noexcept {
    g_live_groups.fetch_add(1, std::memory_order_relaxed);
}

GroupState::~GroupState() {
    // The group makes no call after this, so its providers free what they hold
    // for it instead of waiting for messages that never come. The collectives
    // go first, since their abort of the group leaves an end the messaging
    // would keep for later messages of it.
    backend->collectives->release(id);
    backend->bulk->release(id);
    backend->messaging->release(id);
    {
        std::lock_guard<std::mutex> lock(backend->groups_mutex);
        backend->live_groups.erase(id);
        for (auto at = backend->early_aborts.begin();
             at != backend->early_aborts.end();) {
            if (at->second.parent == id)
                at = backend->early_aborts.erase(at);
            else
                ++at;
        }
    }
    g_live_groups.fetch_sub(1, std::memory_order_relaxed);
}

dstd::StringView call_name(CallKind kind) noexcept {
    switch (kind) {
        case CallKind::Barrier:
            return "barrier";
        case CallKind::MonitoredBarrier:
            return "monitored_barrier";
        case CallKind::Split:
            return "split";
        case CallKind::Broadcast:
            return "broadcast";
        case CallKind::Gather:
            return "gather";
        case CallKind::Scatter:
            return "scatter";
        case CallKind::AllGather:
            return "all_gather";
        case CallKind::Reduce:
            return "reduce";
        case CallKind::AllReduce:
            return "all_reduce";
        case CallKind::AllToAll:
            return "all_to_all";
        case CallKind::ReduceScatter:
            return "reduce_scatter";
        case CallKind::Scan:
            return "scan";
        case CallKind::ExclusiveScan:
            return "exclusive_scan";
        case CallKind::Neighbors:
            return "neighbors";
        case CallKind::NeighborExchange:
            return "neighbor_exchange";
        case CallKind::ChannelSend:
            return "channel_send";
        case CallKind::ChannelRecv:
            return "channel_recv";
        case CallKind::Pull:
            return "pull";
        case CallKind::Push:
            return "push";
    }
    return "unknown";
}

std::uint64_t split_child_id(std::uint64_t parent, std::uint64_t seq,
                             std::uint32_t ordinal) noexcept {
    std::uint64_t x = parent * 0x9E3779B97F4A7C15ull;
    x ^= (seq + 1) * 0xC2B2AE3D27D4EB4Full;
    x ^= (std::uint64_t{ordinal} + 1) * 0x165667B19E3779F9ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x | (std::uint64_t{1} << 63);
}

dstd::SharedPtr<GroupState> make_group_state(
    const dstd::SharedPtr<Backend>& backend,
    dstd::Vector<std::uint32_t> members, std::uint64_t id,
    std::uint64_t parent) {
    auto state = dstd::make_shared<GroupState>();
    state->backend = backend;
    state->parent = parent;
    const std::size_t size = members.size();
    state->next_seq.resize(size, 0);
    state->default_timeout_ns.resize(size, 0);
    state->members = std::move(members);
    state->processes.reserve(size);
    for (const std::uint32_t world_rank : state->members) {
        const std::uint32_t process =
            backend->bootstrap->process_of(world_rank);
        if (!state->processes.empty() && process != state->processes[0])
            state->single_process = false;
        state->processes.push_back(process);
    }
    {
        // The id is taken and the group registered under one lock, which
        // peer_abort also holds: it sees the group, or an id not made yet.
        std::lock_guard<std::mutex> lock(backend->groups_mutex);
        state->id = id != 0 ? id
                            : backend->next_group_id.fetch_add(
                                  1, std::memory_order_relaxed);
        backend->live_groups[state->id] = state;
        const auto early = backend->early_aborts.find(state->id);
        if (early != backend->early_aborts.end()) {
            state->aborted = std::move(early->second.why);
            backend->early_aborts.erase(early);
        }
        state->tally = dstd::make_shared<Tally>(backend->observer, state->id);
        // Under the same lock, so a process binds its groups in the order it
        // made them.
        const dstd::Span<const std::uint32_t> ranks(state->members.data(),
                                                    state->members.size());
        backend->messaging->bind_group(state->id, ranks);
        backend->collectives->bind_group(state->id, ranks);
        backend->bulk->bind_group(state->id, ranks);
    }
    return state;
}

namespace {

/// One error for every member of the group, naming the rank that caused it.
/// The inner text is copied because an @ref Error holds one block; building
/// the whole message here keeps the one prefix in one place.
Error abort_error(const GroupState& group, std::uint32_t aborter,
                  const Error& why) {
    return Error::from_format<"group {}: aborted by rank {}: {}">(
        why.condition(), group.id, aborter, why.message());
}

/// `1, 3` for the members that had (@p arrived_ones) or had not arrived at a
/// slot, `none` for no one, and why this rank cannot tell.
dstd::String listed_ranks(const backend::Arrival& arrived, bool arrived_ones) {
    if (arrived.empty()) return dstd::String("not known to this rank");
    dstd::Vector<std::uint32_t> listed;
    for (std::uint32_t rank = 0; rank < arrived.size(); ++rank)
        if ((arrived[rank] != 0) == arrived_ones) listed.push_back(rank);
    if (listed.empty()) return dstd::String("none");
    dstd::String text;
    dstd::fmt_append_join<", ">(text, listed);
    return text;
}

/// What an abort owes once the group mutex is free: the provider slots to end
/// and the error to fail their calls with.
///
/// Ending a provider slot makes the messaging fail the messages still parked
/// for that group, and those completions end other slots of the group and take
/// the group mutex again, so none of it may run under that mutex.
struct AbortTail {
    dstd::Map<std::uint64_t, dstd::SharedPtr<Slot>> slots;
    dstd::SharedPtr<const Error> why;
};

/// Fails every pending call of @p group and refuses the later ones. The first
/// abort wins. Called with the group mutex held; it only takes the slots out
/// and gives them to @p tail, which the caller ends outside the lock with
/// @ref finish_abort.
void abort_locked(GroupState& group, Error why, AbortTail& tail) {
    if (group.aborted) return;
    group.aborted = std::move(why);
    tail.why = dstd::make_shared<const Error>(*group.aborted);
    tail.slots = std::move(group.slots);
    group.slots = decltype(group.slots){};
}

/// Ends what an abort took out of a group, with the group mutex free. Every
/// provider slot ends before the first call fails, so a completion a slot ends
/// on this thread finds the calls still pending and takes a free mutex.
void finish_abort(const dstd::SharedPtr<Backend>& backend, std::uint64_t id,
                  std::uint64_t parent, AbortTail& tail, bool announce = true) {
    if (!tail.why) return;
    // The parked calls of the channels of the group end with the collectives.
    backend->bulk->abort(id, *tail.why);
    backend->messaging->abort(id, *tail.why);
    if (announce) backend->messaging->announce_abort(id, parent, *tail.why);
    for (auto& entry : tail.slots)
        backend->collectives->drop(id, entry.first, nullptr);
    for (auto& entry : tail.slots)
        for (auto& op : entry.second->ops)
            if (op) op->fail(*tail.why);
}

/// Called on the timer thread when the deadline of one call passes: the group
/// aborts, so every member ends with the same error and none waits alone.
/// @p target holds the call and the group, and gives nothing once the call
/// ended, so a timeout that a finished call no longer needs does nothing.
void expire(const dstd::SharedPtr<TimerTarget>& target, std::uint32_t rank,
            CallKind kind, std::uint64_t seq, std::uint64_t deadline_ns) {
    const ExpireSignal signal;
    dstd::SharedPtr<GroupState> group;
    dstd::SharedPtr<PendingOp> op;
    if (!target->take(group, op)) return;
    backend::Arrival arrived;
    const bool names_missing =
        kind == CallKind::MonitoredBarrier || carries_data(kind);
    if (names_missing)
        // Outside the group mutex: ending the slot runs its parked messages.
        group->backend->collectives->drop(group->id, seq, &arrived);
    if (void (*gate)() = g_expire_gate.load(std::memory_order_acquire)) gate();
    g_expire_at_lock.store(true, std::memory_order_release);
    AbortTail tail;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        // Read under the group mutex: a call that ended in between keeps its
        // outcome, and does not abort its peers.
        if (!op || op->is_done() || group->aborted) return;
        Error why;
        const auto slot = group->slots.find(seq);
        const std::uint64_t age_ns = call_age_ns(
            op->meta(),
            slot != group->slots.end() ? slot->second->start_ns : 0);
        if (names_missing) {
            why = Error::from_format<
                "{} at seq {}: deadline {} passed after {} ns; arrived ranks: "
                "{}; missing ranks: {}">(
                Condition::Timeout, call_name(kind), seq, deadline_ns, age_ns,
                listed_ranks(arrived, true), listed_ranks(arrived, false));
        } else {
            why = Error::from_format<
                "{} at seq {}: deadline {} passed after {} ns">(
                Condition::Timeout, call_name(kind), seq, deadline_ns, age_ns);
        }
        abort_locked(*group, abort_error(*group, rank, why), tail);
    }
    finish_abort(group->backend, group->id, group->parent, tail);
}

/// The handshake: the members of a group must speak one protocol version.
bool check_handshake(const GroupState& group,
                     dstd::Span<const backend::Record> records, Error& why) {
    for (std::uint32_t rank = 1; rank < records.size(); ++rank) {
        if (records[rank].version == records[0].version) continue;
        why = Error::from_format<
            "group {}: rank {} speaks protocol {} but rank {} speaks {}">(
            Condition::Unsupported, group.id, 0, records[0].version, rank,
            records[rank].version);
        return false;
    }
    return true;
}

/// Two members of one slot must make the same call. The lowest member that
/// arrived is the base of the comparison, so every rank that reports the same
/// slot names the same pair.
struct Disagreement {
    std::uint32_t base = 0;
    /// The first member that names another call, or 0 when all agree.
    std::uint32_t other = 0;
};

/// Reads only the members @p seen marks, which is every member of a full
/// slot, so a slot a provider ended early reports its disagreement too.
Disagreement disagreement(dstd::Span<const backend::Record> records,
                          dstd::Span<const std::uint8_t> seen) {
    Disagreement out;
    bool have_base = false;
    for (std::uint32_t rank = 0; rank < records.size(); ++rank) {
        if (!seen.empty() && !seen[rank]) continue;
        if (!have_base) {
            out.base = rank;
            have_base = true;
            continue;
        }
        if (records[rank].kind == records[out.base].kind) continue;
        out.other = rank;
        return out;
    }
    return out;
}

/**
 * Completes every call of a slot, or aborts the group when the members
 * disagree on the call they made or on the protocol. @p T is the value the call
 * gives, which the kind fixes. The first completion of a slot ends it, since a
 * provider may report one slot more than once.
 */
template <class T>
void finish_call(Slot& slot, const backend::SlotEnd& end) {
    const dstd::SharedPtr<GroupState> group = slot.group.lock();
    if (!group) return;
    AbortTail tail;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        decide_call<T>(slot, end, *group, tail);
    }
    finish_abort(group->backend, group->id, group->parent, tail);
}

/// The good ends of the calls of @p slot that the group counts, as one count:
/// a call of a slot does not count its own, so the group pays for one count and
/// not one for each member. @p wanted names the ranks whose calls end well. It
/// runs before they end, so no waiter reads the counters ahead of it.
template <class Keep>
void count_slot(const Slot& slot, Tally* tally, Keep&& wanted, bool locked) {
    CallKind kind = CallKind::Barrier;
    std::uint64_t ok = 0;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    for (std::uint32_t rank = 0; rank < slot.ops.size(); ++rank) {
        const dstd::SharedPtr<PendingOp>& call = slot.ops[rank];
        if (!call || call->is_done() || !call->meta().batched || !wanted(rank))
            continue;
        kind = call->meta().kind;
        ++ok;
        sent += call->sent();
        received += call->received();
    }
    if (ok == 0) return;
    if (locked)
        count_batch_locked(*tally, kind, ok, sent, received);
    else
        count_batch(*tally, kind, ok, sent, received);
}

/// The checks every completion passes, with the group mutex held: the slot is
/// the first to end, the group did not abort, and the members agree on the call
/// and on the protocol. Gives false when the completion is over, which is also
/// when it gave @p group and its slots to @p tail.
bool accept_slot(Slot& slot, const backend::SlotEnd& end, GroupState& group,
                 AbortTail& tail) {
    if (slot.finished.exchange(true)) return false;
    if (group.aborted) return false;
    if (end.why != nullptr) {
        abort_locked(group, abort_error(group, end.rank, *end.why), tail);
        return false;
    }
    const Disagreement broken = disagreement(end.records, end.seen);
    if (broken.other != 0) {
        const Error inner = Error::from_format<
            "seq {}: rank {} called {} but rank {} called {}">(
            Condition::InvalidArgument, slot.seq, broken.base,
            call_name(static_cast<CallKind>(end.records[broken.base].kind)),
            broken.other,
            call_name(static_cast<CallKind>(end.records[broken.other].kind)));
        abort_locked(group, abort_error(group, broken.other, inner), tail);
        return false;
    }
    if (!end.seen.empty()) {
        // The members that did arrive agree, so the provider gave up on one
        // that is not coming.
        const Error inner = Error::from_format<
            "seq {}: the provider ended a slot before every member arrived">(
            Condition::Internal, slot.seq);
        abort_locked(group, abort_error(group, end.rank, inner), tail);
        return false;
    }
    if (end.records.size() != group.size()) {
        const Error inner =
            Error::from_format<"seq {}: a slot of {} members gave {} records">(
                Condition::Internal, slot.seq, group.size(),
                end.records.size());
        abort_locked(group, abort_error(group, end.rank, inner), tail);
        return false;
    }
    Error why;
    if (!check_handshake(group, end.records, why)) {
        abort_locked(group, std::move(why), tail);
        return false;
    }
    return true;
}

/// The decision of one completion, with the group mutex held: completes the
/// calls of @p slot, or gives @p group and its slots to @p tail.
template <class T>
void decide_call(Slot& slot, const backend::SlotEnd& end, GroupState& group,
                 AbortTail& tail) {
    if (!accept_slot(slot, end, group, tail)) return;
    const std::uint32_t size = group.size();
    count_slot(
        slot, group.tally.get(), [](std::uint32_t) { return true; }, true);
    if constexpr (std::is_void_v<T>) {
        for (std::uint32_t member = 0; member < size; ++member) {
            if (!slot.ops[member]) continue;
            auto* op = static_cast<OpState<T>*>(slot.ops[member].get());
            op->succeed();
        }
    } else {
        struct Colored {
            std::uint32_t rank;
            std::uint32_t color;
            std::uint32_t key;
        };
        dstd::Vector<Colored> colored;
        for (std::uint32_t member = 0; member < size; ++member) {
            if (end.records[member].color == Group::NO_COLOR) continue;
            colored.push_back(Colored{member, end.records[member].color,
                                      end.records[member].key});
        }
        std::sort(colored.begin(), colored.end(),
                  [](const Colored& a, const Colored& b) {
                      if (a.color != b.color) return a.color < b.color;
                      if (a.key != b.key) return a.key < b.key;
                      return a.rank < b.rank;
                  });
        std::size_t begin = 0;
        std::uint32_t ordinal = 0;
        while (begin < colored.size()) {
            std::size_t stop = begin + 1;
            while (stop < colored.size() &&
                   colored[stop].color == colored[begin].color) {
                ++stop;
            }
            dstd::Vector<std::uint32_t> members;
            for (std::size_t at = begin; at < stop; ++at)
                members.push_back(group.members[colored[at].rank]);
            dstd::SharedPtr<GroupState> child = make_group_state(
                group.backend, std::move(members),
                split_child_id(group.id, slot.seq, ordinal++), group.id);
            for (std::size_t at = begin; at < stop; ++at) {
                // A rank of another process has no call here to end.
                if (!slot.ops[colored[at].rank]) continue;
                auto* op = static_cast<OpState<Group>*>(
                    slot.ops[colored[at].rank].get());
                op->succeed(Group{group.backend, child,
                                  static_cast<std::uint32_t>(at - begin)});
            }
            begin = stop;
        }
        for (std::uint32_t member = 0; member < size; ++member) {
            if (end.records[member].color != Group::NO_COLOR) continue;
            if (!slot.ops[member]) continue;
            auto* op = static_cast<OpState<Group>*>(slot.ops[member].get());
            op->succeed(Group{});
        }
    }
    const auto at = group.slots.find(slot.seq);
    if (at != group.slots.end() && at->second.get() == &slot)
        group.slots.erase(at);
}

/// Ends the calls of a slot with the end the provider gave.
using FinishFn = void (*)(Slot&, const backend::SlotEnd&);

/**
 * Records the call of @p rank at its next sequence number and arrives at the
 * provider. The call is registered before anything can fail it, so an abort
 * reaches it like the calls of the peers. @p finisher and @p fold, when given,
 * are those of a data call and move into the slot.
 */
void start_call(const dstd::SharedPtr<Backend>& backend,
                const dstd::SharedPtr<GroupState>& group, std::uint32_t rank,
                CallKind kind, backend::Record record,
                backend::Contribution&& contribution, const CallOpts& opts,
                const dstd::SharedPtr<PendingOp>& state, FinishFn finish,
                Finisher* finisher, backend::Combine* fold,
                dstd::SharedPtr<const void>* object) {
    if (opts.stop.stop_requested()) (void)cancel_group(*group, rank);

    // Before the group mutex: the ranks of a group queue on it, and what is
    // done under it is paid by every one of them.
    state->observe(
        make_meta(group->tally.get(), kind, group->members[rank], true));
    state->set_sent(contribution.bytes.size());

    dstd::SharedPtr<Slot> slot;
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        count_start_locked(*group->tally, kind);
        if (group->aborted) {
            state->fail(*group->aborted);
            return;
        }
        seq = group->next_seq[rank]++;
        state->set_seq(seq);
        std::uint64_t deadline = opts.deadline_ns;
        coro::TimerService* timers = backend->bootstrap->timers();
        const bool timed =
            deadline != 0 || group->default_timeout_ns[rank] != 0;
        if (timed) {
            const std::uint64_t now = dstd::steady_now_ns();
            state->set_start_ns(now);
            if (deadline == 0) deadline = now + group->default_timeout_ns[rank];
            if (timers != nullptr) {
                const std::chrono::nanoseconds delay{
                    deadline > now ? deadline - now : 0};
                auto target = dstd::make_shared<detail::TimerTarget>();
                target->group = group;
                target->op = state;
                const coro::TimerService::TimerId timer =
                    timers->register_timeout(
                        delay, [target, rank, kind, seq, deadline]() {
                            expire(target, rank, kind, seq, deadline);
                        });
                state->set_timeout(timers, timer, std::move(target));
            }
        }

        auto& holder = group->slots[seq];
        if (!holder) {
            // The group of a slot is written once, under this mutex, so a
            // completion reads it without the mutex with no other order.
            holder = dstd::make_shared<Slot>(seq, group->size());
            holder->group = group;
            holder->start_ns = dstd::coarse_now_ns();
        }
        slot = holder;
        slot->ops[rank] = state;
        if (finisher != nullptr) {
            if (slot->finishers.empty()) {
                slot->finishers.resize(group->size());
                slot->folds.resize(group->size());
            }
            slot->finishers[rank] = std::move(*finisher);
            if (object != nullptr && *object) {
                if (slot->objects.empty()) slot->objects.resize(group->size());
                slot->objects[rank] = std::move(*object);
            }
            if (fold != nullptr && *fold) {
                if ((record.flags & backend::RECORD_COMBINE) != 0)
                    contribution.combine = std::move(*fold);
                else
                    slot->folds[rank] = std::move(*fold);
            }
        }
    }
    // Outside the group mutex: a local provider completes the call on this
    // thread, and the completion takes that mutex.
    record.kind = static_cast<std::uint32_t>(kind);
    record.version = backend->protocol_version[group->members[rank]];
    const std::uint64_t group_id = group->id;
    const std::uint32_t members = group->size();
    const dstd::SharedPtr<backend::Collectives> collectives =
        backend->collectives;
    // Two words for one slot and a function pointer, so the completion fits
    // the inline buffer of the function and a call allocates nothing for it.
    auto notify = [slot, finish](const backend::SlotEnd& end) {
        finish(*slot, end);
    };
    if (collectives->arrive(group_id, seq, members, rank, record,
                            std::move(contribution), std::move(notify)) &&
        state->is_done()) {
        // The group aborted between the registration and the arrival, so that
        // abort did not see this slot. A later abort never does.
        collectives->drop(group_id, seq, nullptr);
    }
}

template <class T>
Op<T> make_call(const dstd::SharedPtr<Backend>& backend,
                const dstd::SharedPtr<GroupState>& group, std::uint32_t rank,
                CallKind kind, std::uint32_t color, std::uint32_t key,
                const CallOpts& opts) {
    auto state = dstd::make_shared<OpState<T>>();
    if (!group) {
        if constexpr (std::is_void_v<T>) {
            state->succeed();
        } else {
            state->succeed(T{});
        }
        return Op<T>(std::move(state), nullptr, rank);
    }
    backend::Record record;
    record.color = color;
    record.key = key;
    start_call(backend, group, rank, kind, record, backend::Contribution{},
               opts, state, &finish_call<T>, nullptr, nullptr, nullptr);
    return Op<T>(std::move(state), group, rank);
}

Error shape_error(std::uint64_t seq, dstd::StringView field, std::uint32_t rank,
                  std::uint64_t value, std::uint32_t base,
                  std::uint64_t base_value) {
    return Error::from_format<"seq {}: rank {} gave {} {} but rank {} gave {}">(
        Condition::InvalidArgument, seq, rank, field, value, base, base_value);
}

/// Whether the packed block of a scatter holds @p members pieces whose sizes
/// add up to the rest of the block.
bool packed_block_fits(dstd::Span<const std::byte> block,
                       std::uint32_t members) {
    const std::uint64_t head = std::uint64_t{members} * sizeof(std::uint64_t);
    if (block.size() < head) return false;
    std::uint64_t total = 0;
    for (std::uint32_t at = 0; at < members; ++at)
        total += packed_size(block, at);
    return total == block.size() - head;
}

/// The checks of the calls that split values into one block per rank, which
/// the records allow: the counts divide, and the ranks that send rows or
/// blocks to neighbors name valid ones. Gives false with the error and the rank
/// that names it.
bool check_split(const Slot& slot, const backend::SlotEnd& end, CallKind kind,
                 std::uint32_t size, std::uint32_t& who, Error& why) {
    const backend::Record& base = end.records[0];
    who = 0;
    switch (kind) {
        case CallKind::AllToAll:
            if (base.color == ALL_TO_ALL_EQUAL && base.count % size != 0) {
                why = Error::from_format<
                    "seq {}: the {} values of each rank do not split into {} "
                    "blocks">(Condition::InvalidArgument, slot.seq, base.count,
                              size);
                return false;
            }
            if ((base.color == ALL_TO_ALL_BLOCKS ||
                 base.color == OBJECT_SHARED) &&
                base.count != size) {
                why = Error::from_format<
                    "seq {}: the ranks gave {} blocks for a group of {}">(
                    Condition::InvalidArgument, slot.seq, base.count, size);
                return false;
            }
            if (base.color == ALL_TO_ALL_ROWS) {
                for (std::uint32_t rank = 0; rank < size; ++rank) {
                    if (end.records[rank].key == 0) continue;
                    who = rank;
                    why = Error::from_format<
                        "seq {}: rank {} sent a row to rank {}, which is not "
                        "below the group size {}">(
                        Condition::InvalidArgument, slot.seq, rank,
                        end.records[rank].key - 1, size);
                    return false;
                }
            }
            return true;
        case CallKind::ReduceScatter:
            if (base.key == SPLIT_BAD) {
                why = Error::from_format<
                    "seq {}: the counts of the ranks do not split {} values "
                    "into {} blocks">(Condition::InvalidArgument, slot.seq,
                                      base.count, size);
                return false;
            }
            if (base.key == SPLIT_EQUAL && base.count % size != 0) {
                why = Error::from_format<
                    "seq {}: the {} values of each rank do not split into {} "
                    "blocks">(Condition::InvalidArgument, slot.seq, base.count,
                              size);
                return false;
            }
            return true;
        case CallKind::NeighborExchange:
            for (std::uint32_t rank = 0; rank < size; ++rank) {
                if (end.records[rank].count == end.records[rank].key) continue;
                who = rank;
                why = Error::from_format<
                    "seq {}: rank {} gave {} blocks for {} out-neighbors">(
                    Condition::InvalidArgument, slot.seq, rank,
                    end.records[rank].count, end.records[rank].key);
                return false;
            }
            return true;
        default:
            return true;
    }
}

/// The checks of a data call that the records and the table allow once every
/// member arrived: the members agree on the root, the element type, the count
/// where it must match and the merge, and the root's data fits a scatter.
/// Gives false with the error and the rank that names it.
bool check_shape(const GroupState& group, const Slot& slot,
                 const backend::SlotEnd& end, std::uint32_t& who, Error& why) {
    const CallKind kind = static_cast<CallKind>(end.records[0].kind);
    const std::uint32_t size = group.size();
    const backend::Record& base = end.records[0];
    // A member that did not ask for the table may end the slot with none.
    const bool has_table = !end.table.offsets.empty() || !end.tables.empty();
    if (!has_table &&
        (end.records[end.rank].flags & backend::RECORD_WANTS) != 0) {
        who = end.rank;
        why = Error::from_format<"seq {}: the provider gave no table">(
            Condition::Internal, slot.seq);
        return false;
    }
    for (std::uint32_t rank = 1; rank < size; ++rank) {
        const backend::Record& other = end.records[rank];
        who = rank;
        if (other.root != base.root) {
            why = shape_error(slot.seq, "root", rank, other.root, 0, base.root);
            return false;
        }
        if (other.elem_size != base.elem_size) {
            why = shape_error(slot.seq, "element size", rank, other.elem_size,
                              0, base.elem_size);
            return false;
        }
        if (kind != CallKind::Scatter && kind != CallKind::NeighborExchange &&
            other.count != base.count) {
            why = shape_error(slot.seq, "count", rank, other.count, 0,
                              base.count);
            return false;
        }
        if (other.color != base.color) {
            why = shape_error(slot.seq, "layout", rank, other.color, 0,
                              base.color);
            return false;
        }
        if (other.reduce != base.reduce) {
            why = shape_error(slot.seq, "reduce op", rank, other.reduce, 0,
                              base.reduce);
            return false;
        }
        const std::uint32_t order = other.flags & backend::RECORD_COMBINE;
        if (order != (base.flags & backend::RECORD_COMBINE)) {
            why = shape_error(slot.seq, "reduce order", rank, order, 0,
                              base.flags & backend::RECORD_COMBINE);
            return false;
        }
        if ((kind == CallKind::ReduceScatter || base.color == OBJECT_SHARED) &&
            other.key != base.key) {
            why = shape_error(
                slot.seq,
                kind == CallKind::ReduceScatter ? "block split" : "object type",
                rank, other.key, 0, base.key);
            return false;
        }
    }
    who = base.root;
    if (kind != CallKind::AllGather && kind != CallKind::AllReduce &&
        base.root >= size) {
        why = Error::from_format<
            "seq {}: root {} is not below the group size {}">(
            Condition::InvalidArgument, slot.seq, base.root, size);
        return false;
    }
    if (!check_split(slot, end, kind, size, who, why)) return false;
    if (kind == CallKind::Scatter && has_table) {
        const backend::Record& root = end.records[base.root];
        const dstd::Span<const std::byte> block = end.table.at(base.root);
        const bool fits =
            root.color == SCATTER_PACKED
                ? root.count == size && packed_block_fits(block, size)
                : root.count % size == 0 &&
                      block.size() == root.count * root.elem_size;
        if (!fits) {
            why = Error::from_format<
                "seq {}: the {} values of root {} do not split into {} "
                "pieces">(Condition::InvalidArgument, slot.seq, root.count,
                          base.root, size);
            return false;
        }
    }
    return true;
}

/// The outcome of a data call: the table of the provider, or for a reduction
/// that the group merges, the fold of its entries in rank order.
Outcome make_outcome(const backend::SlotEnd& end,
                     dstd::Span<const backend::Combine> folds) {
    Outcome outcome;
    outcome.table = end.table;
    const backend::Record& base = end.records[0];
    // A scan keeps every entry, each rank merges its own prefix.
    const CallKind kind = static_cast<CallKind>(base.kind);
    if (base.reduce == REDUCE_NONE || kind == CallKind::Scan ||
        kind == CallKind::ExclusiveScan || end.folded ||
        (base.flags & backend::RECORD_COMBINE) != 0 ||
        end.table.offsets.empty())
        return outcome;
    // The merge of the root, or of rank 0 for an all_reduce, whose record names
    // root 0; any other rank's only when that one is not in this process.
    const backend::Combine* fold = nullptr;
    if (base.root < folds.size() && folds[base.root]) fold = &folds[base.root];
    for (const backend::Combine& candidate : folds) {
        if (fold != nullptr) break;
        if (candidate) fold = &candidate;
    }
    auto buffer = dstd::make_shared<backend::TableBuffer>();
    const dstd::Span<const std::byte> first = end.table.at(0);
    buffer->data.assign(first.begin(), first.end());
    if (fold != nullptr) {
        for (std::uint32_t rank = 1; rank < end.records.size(); ++rank) {
            const dstd::Span<const std::byte> in = end.table.at(rank);
            if (in.size() == buffer->data.size())
                (*fold)(buffer->data.data(), in.data(), in.size());
        }
    }
    buffer->offsets.push_back(0);
    buffer->offsets.push_back(buffer->data.size());
    outcome.table = backend::make_table(std::move(buffer));
    return outcome;
}

/// Completes the calls of a data slot, or aborts the group when the members
/// disagree. The group mutex covers only the decision: the merge and the ends
/// of the calls run after it, so a user function never runs under it.
void finish_data(Slot& slot, const backend::SlotEnd& end) {
    const dstd::SharedPtr<GroupState> group = slot.group.lock();
    if (!group) return;
    AbortTail tail;
    dstd::Vector<Finisher> finishers;
    dstd::Vector<backend::Combine> folds;
    dstd::Vector<dstd::SharedPtr<const void>> objects;
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        if (accept_slot(slot, end, *group, tail)) {
            std::uint32_t who = 0;
            Error why;
            if (check_shape(*group, slot, end, who, why)) {
                finishers = std::move(slot.finishers);
                folds = std::move(slot.folds);
                objects = std::move(slot.objects);
                claimed = true;
                const auto at = group->slots.find(slot.seq);
                if (at != group->slots.end() && at->second.get() == &slot)
                    group->slots.erase(at);
            } else {
                abort_locked(*group, abort_error(*group, who, why), tail);
            }
        }
    }
    finish_abort(group->backend, group->id, group->parent, tail);
    if (!claimed) return;
    Outcome outcome = make_outcome(end, folds);
    outcome.objects = &objects;
    DecodedObjects decoded;
    outcome.decoded = &decoded;
    outcome.processes = dstd::Span<const std::uint32_t>(
        group->processes.data(), group->processes.size());
    // A provider that gives each member its own table serves only the members
    // it knows; the others cannot end with data.
    const bool personal = !end.tables.empty();
    dstd::Optional<Error> unusable;
    std::uint32_t unusable_rank = 0;
    // Every rank stages its value first, so a rank that cannot use the outcome
    // fails the calls of all ranks of the slot, and none ends with a value.
    for (std::uint32_t rank = 0; rank < finishers.size() && !unusable; ++rank) {
        // A rank that is not in this process has no call here to end.
        if (!finishers[rank] || !slot.ops[rank]) continue;
        Outcome mine;
        mine.objects = &objects;
        mine.decoded = &decoded;
        mine.processes = outcome.processes;
        if (personal) {
            if (end.tables[rank].offsets.empty()) {
                unusable = Error::from_format<
                    "seq {}: the provider gave no table for rank {}">(
                    Condition::Internal, slot.seq, rank);
                unusable_rank = rank;
                break;
            }
            mine.table = end.tables[rank];
        }
        const Outcome& taken = personal ? mine : outcome;
        dstd::Optional<Error> why = finishers[rank](taken);
        if (why) {
            unusable = std::move(why);
            unusable_rank = rank;
        } else if ((end.records[rank].flags & backend::RECORD_WANTS) != 0) {
            slot.ops[rank]->set_received(taken.table.data.size());
        }
    }
    if (!unusable) {
        count_slot(
            slot, group->tally.get(),
            [&finishers](std::uint32_t rank) {
                return static_cast<bool>(finishers[rank]);
            },
            false);
        for (std::uint32_t rank = 0; rank < finishers.size(); ++rank)
            if (finishers[rank] && slot.ops[rank]) slot.ops[rank]->commit();
        return;
    }
    AbortTail after;
    Error shared;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        abort_locked(*group, abort_error(*group, unusable_rank, *unusable),
                     after);
        shared = *group->aborted;
    }
    for (std::uint32_t rank = 0; rank < finishers.size(); ++rank)
        if (finishers[rank] && slot.ops[rank]) slot.ops[rank]->fail(shared);
    finish_abort(group->backend, group->id, group->parent, after);
}

/// Runs one abort on the executor. The frame holds the group until it runs, so
/// a cancelled call still aborts the peers on this rank.
class AbortJob {
   public:
    struct promise_type {
        AbortJob get_return_object() noexcept {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
    std::coroutine_handle<promise_type> handle;
};

AbortJob run_abort(dstd::SharedPtr<GroupState> group, std::uint32_t rank) {
    (void)cancel_group(*group, rank);
    co_return;
}

}  // namespace

dstd::Error cancel_group(GroupState& group, std::uint32_t rank) noexcept {
    AbortTail tail;
    Error out;
    {
        std::lock_guard<std::mutex> lock(group.mutex);
        abort_locked(group, abort_error(group, rank, canceled_call()), tail);
        out = group.aborted ? *group.aborted : canceled_call();
    }
    finish_abort(group.backend, group.id, group.parent, tail);
    return out;
}

void cancel_group(GroupState& group, const PendingOp* op,
                  std::uint32_t rank) noexcept {
    AbortTail tail;
    {
        std::lock_guard<std::mutex> lock(group.mutex);
        // Read under the group mutex: a call that ended in between keeps its
        // outcome and does not abort its peers.
        if (op != nullptr && op->is_done()) return;
        abort_locked(group, abort_error(group, rank, canceled_call()), tail);
    }
    finish_abort(group.backend, group.id, group.parent, tail);
}

void peer_abort(const dstd::SharedPtr<Backend>& backend, std::uint64_t id,
                std::uint64_t parent, const Error& why) {
    dstd::SharedPtr<GroupState> group;
    {
        std::lock_guard<std::mutex> lock(backend->groups_mutex);
        const auto at = backend->live_groups.find(id);
        if (at != backend->live_groups.end()) group = at->second.lock();
        if (!group) {
            // Ids in sequence only grow, so one below the next id was made and
            // is gone. A child of a split has a derived id: its abort is kept
            // for its birth only while its parent is alive here, and goes with
            // the parent. A process outside the parent keeps nothing.
            const bool needed = parent != 0
                                    ? backend->live_groups.count(parent) != 0
                                    : id >= backend->next_group_id.load(
                                                std::memory_order_relaxed);
            if (needed)
                backend->early_aborts.emplace(id, EarlyAbort{why, parent});
            return;
        }
    }
    AbortTail tail;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        abort_locked(*group, why, tail);
    }
    finish_abort(backend, id, group->parent, tail, false);
}

void abandon_group(const dstd::SharedPtr<GroupState>& group, std::uint32_t rank,
                   coro::Executor* executor) noexcept {
    if (!group) return;
    if (executor == nullptr) {
        (void)cancel_group(*group, rank);
        return;
    }
    AbortJob job = run_abort(group, rank);
    executor->post(job.handle);
}

}  // namespace detail

std::uint32_t Group::size() const noexcept {
    return state_ ? state_->size() : 0;
}

std::uint64_t Group::id() const noexcept { return state_ ? state_->id : 0; }

bool Group::in_one_process() const noexcept {
    return !state_ || state_->single_process;
}

bool Group::same_process(std::uint32_t other) const noexcept {
    return state_->processes[other] == state_->processes[rank_];
}

Op<Group> Group::split(std::uint32_t color, std::uint32_t key, CallOpts opts) {
    return detail::make_call<Group>(backend_, state_, rank_,
                                    detail::CallKind::Split, color, key, opts);
}

Op<void> Group::barrier(CallOpts opts) {
    return detail::make_call<void>(backend_, state_, rank_,
                                   detail::CallKind::Barrier, 0, 0, opts);
}

Op<void> Group::monitored_barrier(CallOpts opts) {
    return detail::make_call<void>(backend_, state_, rank_,
                                   detail::CallKind::MonitoredBarrier, 0, 0,
                                   opts);
}

void Group::start_data(detail::DataCall call, detail::Finisher finisher,
                       const dstd::SharedPtr<PendingOp>& state,
                       const CallOpts& opts) {
    detail::start_call(backend_, state_, rank_, call.kind, call.record,
                       std::move(call.contribution), opts, state,
                       &detail::finish_data, &finisher, &call.fold,
                       &call.object);
}

Op<Gathered> Group::table_call(detail::DataCall call, const CallOpts& opts) {
    return data_op<Gathered>(
        std::move(call),
        [](const detail::Outcome& out) { return Gathered(out.table); }, opts);
}

Op<Gathered> Group::all_to_all(
    dstd::Span<const dstd::Span<const std::byte>> blocks, CallOpts opts) {
    detail::DataCall call = detail::make_data_call(
        detail::CallKind::AllToAll, 0, 1, blocks.size(),
        backend::RECORD_WANTS | backend::RECORD_PERSONAL,
        detail::own_personal(size(), [&](std::uint32_t dest) {
            return dest < blocks.size() ? blocks[dest]
                                        : dstd::Span<const std::byte>();
        }));
    call.record.color = detail::ALL_TO_ALL_BLOCKS;
    return table_call(std::move(call), opts);
}

Op<Neighborhood> Group::neighbors(dstd::Span<const std::uint32_t> out_neighbors,
                                  CallOpts opts) {
    detail::DataCall call = detail::make_data_call(
        detail::CallKind::Neighbors, 0, sizeof(std::uint32_t),
        backend::UNCHECKED_COUNT, backend::RECORD_WANTS,
        detail::own_bytes(std::as_bytes(out_neighbors)));
    dstd::Vector<std::uint32_t> mine(out_neighbors.begin(),
                                     out_neighbors.end());
    // Weak, because the slot that holds this call lives in the group.
    dstd::WeakPtr<detail::GroupState> weak = state_;
    return data_op<Neighborhood>(
        std::move(call),
        [owner = backend_, weak = std::move(weak), rank = rank_,
         mine = std::move(mine)](
            const detail::Outcome& out) -> dstd::Result<Neighborhood> {
            const std::uint32_t members = out.table.size();
            dstd::Vector<std::uint32_t> in;
            dstd::Vector<std::uint32_t> stamp(members, 0);
            for (std::uint32_t from = 0; from < members; ++from) {
                const dstd::Span<const std::byte> bytes = out.table.at(from);
                if (bytes.size() % sizeof(std::uint32_t) != 0) {
                    return dstd::Unexpected(
                        Error::from_format<"rank {} named its neighbors in {} "
                                           "bytes, which is not a list of "
                                           "ranks">(Condition::InvalidArgument,
                                                    from, bytes.size()));
                }
                bool names_me = false;
                for (std::size_t at = 0; at < bytes.size();
                     at += sizeof(std::uint32_t)) {
                    std::uint32_t to = 0;
                    std::memcpy(&to, bytes.data() + at, sizeof(to));
                    if (to >= members) {
                        return dstd::Unexpected(
                            Error::from_format<"rank {} names rank {}, which "
                                               "is not below the group size "
                                               "{}">(Condition::InvalidArgument,
                                                     from, to, members));
                    }
                    if (stamp[to] == from + 1) {
                        return dstd::Unexpected(
                            Error::from_format<"rank {} names rank {} twice">(
                                Condition::InvalidArgument, from, to));
                    }
                    stamp[to] = from + 1;
                    if (to == rank) names_me = true;
                }
                if (names_me) in.push_back(from);
            }
            const dstd::SharedPtr<detail::GroupState> state = weak.lock();
            if (!state) {
                return dstd::Unexpected(
                    Error::from_format<"the group ended before its "
                                       "neighbors were fixed">(
                        Condition::Cancelled));
            }
            return dstd::Result<Neighborhood>(Neighborhood(
                Group{owner, state, rank}, mine,
                dstd::make_shared<const dstd::Vector<std::uint32_t>>(
                    std::move(in))));
        },
        opts);
}

Op<NeighborBlocks> Neighborhood::neighbor_exchange(
    dstd::Span<const dstd::Span<const std::byte>> blocks, CallOpts opts) {
    const std::uint32_t members = group_.size();
    dstd::Vector<dstd::Span<const std::byte>> by_dest(members);
    const std::size_t used = std::min(blocks.size(), out_.size());
    for (std::size_t at = 0; at < used; ++at) by_dest[out_[at]] = blocks[at];
    detail::DataCall call = detail::make_data_call(
        detail::CallKind::NeighborExchange, 0, 1, blocks.size(),
        backend::RECORD_WANTS | backend::RECORD_PERSONAL,
        detail::own_personal(
            members, [&](std::uint32_t dest) { return by_dest[dest]; }));
    call.record.key = static_cast<std::uint32_t>(out_.size());
    return group_.data_op<NeighborBlocks>(
        std::move(call),
        [in = in_](const detail::Outcome& out) {
            return NeighborBlocks(Gathered(out.table), in);
        },
        opts);
}

Op<Bytes> Group::broadcast(std::uint32_t root, Bytes value, CallOpts opts) {
    backend::Contribution contribution;
    if (rank_ == root)
        contribution =
            backend::Contribution{value.view, std::move(value.keep), {}};
    detail::DataCall call = detail::make_data_call(
        detail::CallKind::Broadcast, root, 1, backend::UNCHECKED_COUNT,
        backend::RECORD_WANTS, std::move(contribution));
    return data_op<Bytes>(
        std::move(call),
        [root](const detail::Outcome& out) {
            return Bytes{out.table.at(root), out.table.keep};
        },
        opts);
}

Op<Bytes> Group::broadcast(std::uint32_t root,
                           dstd::Span<const std::byte> value, CallOpts opts) {
    return broadcast(root, rank_ == root ? Bytes::copy_of(value) : Bytes{},
                     opts);
}

Op<Gathered> Group::gather_call(detail::CallKind kind, std::uint32_t root,
                                std::uint32_t elem_size, std::uint64_t count,
                                backend::Contribution contribution,
                                const CallOpts& opts) {
    const bool takes = kind == detail::CallKind::AllGather || rank_ == root;
    detail::DataCall call = detail::make_data_call(
        kind, root, elem_size, count, takes ? backend::RECORD_WANTS : 0,
        std::move(contribution));
    return data_op<Gathered>(
        std::move(call),
        [takes](const detail::Outcome& out) {
            return takes ? Gathered(out.table) : Gathered{};
        },
        opts);
}

Op<Gathered> Group::gather(std::uint32_t root, Bytes value, CallOpts opts) {
    return gather_call(
        detail::CallKind::Gather, root, 1, backend::UNCHECKED_COUNT,
        backend::Contribution{value.view, std::move(value.keep), {}}, opts);
}

Op<Gathered> Group::gather(std::uint32_t root,
                           dstd::Span<const std::byte> value, CallOpts opts) {
    return gather(root, Bytes::copy_of(value), opts);
}

Op<Gathered> Group::all_gather(Bytes value, CallOpts opts) {
    return gather_call(
        detail::CallKind::AllGather, 0, 1, backend::UNCHECKED_COUNT,
        backend::Contribution{value.view, std::move(value.keep), {}}, opts);
}

Op<Gathered> Group::all_gather(dstd::Span<const std::byte> value,
                               CallOpts opts) {
    return all_gather(Bytes::copy_of(value), opts);
}

Op<Bytes> Group::scatter(std::uint32_t root,
                         dstd::Span<const dstd::Span<const std::byte>> pieces,
                         CallOpts opts) {
    backend::Contribution contribution;
    std::uint64_t count = 0;
    if (rank_ == root) {
        auto block = dstd::make_shared<dstd::Vector<std::byte>>();
        std::size_t total = pieces.size() * sizeof(std::uint64_t);
        for (const dstd::Span<const std::byte>& piece : pieces)
            total += piece.size();
        block->reserve(total);
        for (const dstd::Span<const std::byte>& piece : pieces) {
            const std::uint64_t size = piece.size();
            const auto raw =
                std::as_bytes(dstd::Span<const std::uint64_t>(&size, 1));
            block->insert(block->end(), raw.begin(), raw.end());
        }
        for (const dstd::Span<const std::byte>& piece : pieces)
            block->insert(block->end(), piece.begin(), piece.end());
        const dstd::Span<const std::byte> view(block->data(), block->size());
        contribution = backend::Contribution{view, std::move(block), {}};
        count = pieces.size();
    }
    detail::DataCall call =
        detail::make_data_call(detail::CallKind::Scatter, root, 1, count,
                               backend::RECORD_WANTS, std::move(contribution));
    call.record.color = detail::SCATTER_PACKED;
    const std::uint32_t rank = rank_;
    return data_op<Bytes>(
        std::move(call),
        [root, rank](const detail::Outcome& out) {
            const dstd::Span<const std::byte> block = out.table.at(root);
            std::uint64_t at =
                std::uint64_t{out.table.size()} * sizeof(std::uint64_t);
            for (std::uint32_t before = 0; before < rank; ++before)
                at += detail::packed_size(block, before);
            return Bytes{block.subspan(at, detail::packed_size(block, rank)),
                         out.table.keep};
        },
        opts);
}

void Group::set_default_deadline(std::uint64_t timeout_ns) noexcept {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->default_timeout_ns[rank_] = timeout_ns;
}

void Group::abort(Error why) noexcept {
    if (!state_) return;
    detail::AbortTail tail;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        detail::abort_locked(*state_, detail::abort_error(*state_, rank_, why),
                             tail);
    }
    detail::finish_abort(state_->backend, state_->id, state_->parent, tail);
}

namespace {

/// One group per node of the run, so the ranks of a node share it. The node of
/// every rank of the local backend is the world, which gives the world group.
dstd::Vector<Group> make_node_groups(
    const dstd::SharedPtr<detail::Backend>& backend,
    const dstd::SharedPtr<detail::GroupState>& world) {
    const std::uint32_t ranks = world->size();
    dstd::Vector<Group> nodes(ranks);
    for (std::uint32_t rank = 0; rank < ranks; ++rank) {
        if (nodes[rank].size() != 0) continue;
        dstd::Vector<std::uint32_t> members =
            backend->bootstrap->node_members(rank);
        dstd::SharedPtr<detail::GroupState> state =
            members == world->members
                ? world
                : detail::make_group_state(backend, members);
        for (std::uint32_t at = 0; at < members.size(); ++at)
            nodes[members[at]] = Group{backend, state, at};
    }
    return nodes;
}

}  // namespace

dstd::Result<Communicator> Communicator::start(Providers providers) {
    if (!providers.bootstrap || !providers.messaging ||
        !providers.collectives) {
        return dstd::Unexpected(dstd::Error::from_format<
                                "a communicator needs a bootstrap, messaging "
                                "and collectives">(Condition::InvalidArgument));
    }
    const std::uint32_t ranks = providers.bootstrap->ranks();
    if (ranks == 0) {
        return dstd::Unexpected(
            dstd::Error::from_format<"a communicator needs at least one rank">(
                Condition::InvalidArgument));
    }
    auto made = dstd::make_shared<detail::Backend>();
    made->bootstrap = std::move(providers.bootstrap);
    made->messaging = std::move(providers.messaging);
    made->collectives = std::move(providers.collectives);
    made->bulk = providers.bulk ? std::move(providers.bulk)
                                : backend::make_message_bulk(made->messaging);
    made->protocol_version.resize(ranks, detail::PROTOCOL_VERSION);
    {
        dstd::SharedPtr<detail::Tracer> trace;
        if (const auto dir = common::Env::get<dstd::StringView>(
                common::constants::comm_settings::COMM_TRACE_ENV))
            trace = detail::Tracer::make(*dir);
        made->observer = dstd::make_shared<detail::Observer>(std::move(trace));
    }
    // Weak: the messaging belongs to the backend, which must not be kept alive
    // by the handler its own provider holds.
    made->messaging->on_peer_abort(
        [weak = dstd::WeakPtr<detail::Backend>(made)](
            std::uint64_t id, std::uint64_t parent, const Error& why) {
            if (const auto backend = weak.lock())
                detail::peer_abort(backend, id, parent, why);
        });
    dstd::Vector<std::uint32_t> members;
    members.reserve(ranks);
    for (std::uint32_t rank = 0; rank < ranks; ++rank) members.push_back(rank);
    Communicator comm{made};
    comm.world_ = detail::make_group_state(made, std::move(members));
    comm.node_ = make_node_groups(made, comm.world_);
    return comm;
}

dstd::Result<Communicator> Communicator::start_local(std::uint32_t ranks,
                                                     coro::Executor& executor) {
    if (ranks == 0) {
        return dstd::Unexpected(
            dstd::Error::from_format<"a communicator needs at least one rank">(
                Condition::InvalidArgument));
    }
    return start(Providers{backend::make_local_bootstrap(ranks, executor),
                           backend::make_local_messaging(),
                           backend::make_local_collectives(),
                           backend::make_local_bulk()});
}

dstd::Result<Communicator> Communicator::start(BackendKind kind,
                                               const StartOptions& options) {
    const bool wanted = kind == BackendKind::Mpi;
    kind = backend::resolve_kind(kind);
    if (kind == BackendKind::Local) {
        return start(Providers{
            backend::make_local_bootstrap(1), backend::make_local_messaging(),
            backend::make_local_collectives(), backend::make_local_bulk()});
    }
    auto loaded = backend::load_backend(options);
    if (!loaded) {
        if (wanted) return dstd::Unexpected(loaded.error());
        return dstd::Unexpected(
            dstd::Error::from_format<
                "a launcher started this process but the MPI backend is "
                "unavailable, so the ranks would run as unrelated copies: {}">(
                loaded.error().condition(), loaded.error().message()));
    }
    return start(std::move(*loaded));
}

namespace {

/// The groups alive on @p backend, so a snapshot reads them without the lock.
dstd::Vector<dstd::SharedPtr<detail::GroupState>> live_group_states(
    detail::Backend& backend) {
    dstd::Vector<dstd::SharedPtr<detail::GroupState>> groups;
    std::lock_guard<std::mutex> lock(backend.groups_mutex);
    for (const auto& entry : backend.live_groups)
        if (auto group = entry.second.lock())
            groups.push_back(std::move(group));
    return groups;
}

}  // namespace

dstd::Vector<CallStats> Communicator::stats() const {
    dstd::Vector<CallStats> rows;
    if (!backend_) return rows;
    for (const auto& group : live_group_states(*backend_))
        detail::add_stats(*group->tally, rows);
    detail::add_retired(*backend_->observer, rows);
    return rows;
}

dstd::Vector<PendingCall> Communicator::pending() const {
    dstd::Vector<PendingCall> calls;
    if (!backend_) return calls;
    for (const auto& group : live_group_states(*backend_)) {
        std::lock_guard<std::mutex> lock(group->mutex);
        for (const auto& entry : group->slots) {
            const detail::Slot& slot = *entry.second;
            for (std::uint32_t rank = 0; rank < slot.ops.size(); ++rank) {
                const auto& op = slot.ops[rank];
                if (!op || op->is_done()) continue;
                const detail::CallMeta& meta = op->meta();
                if (!meta.tally) continue;
                calls.push_back(PendingCall{
                    group->id, detail::call_name(meta.kind), meta.seq, rank,
                    detail::call_age_ns(meta, slot.start_ns)});
            }
        }
    }
    return calls;
}

void detail::Hooks::flush_trace(const Communicator& comm) {
    if (comm.backend_ && comm.backend_->observer->tracer)
        comm.backend_->observer->tracer->flush();
}

Group Communicator::world(std::uint32_t rank) noexcept {
    if (!backend_ || rank >= backend_->ranks())
        dstd::fatal("comm: rank is not below the size of the communicator");
    return Group{backend_, world_, rank};
}

Group Communicator::local(std::uint32_t rank) noexcept {
    if (!backend_ || rank >= node_.size())
        dstd::fatal("comm: rank is not below the size of the communicator");
    return node_[rank];
}

std::uint32_t Communicator::size() const noexcept {
    return backend_ ? backend_->ranks() : 0;
}

std::uint32_t Communicator::rank() const noexcept {
    return backend_ ? backend_->bootstrap->own_rank() : 0;
}

void detail::Hooks::set_protocol_version(Communicator& comm, std::uint32_t rank,
                                         std::uint32_t version) noexcept {
    if (!comm.backend_ || rank >= comm.backend_->ranks()) return;
    comm.backend_->protocol_version[rank] = version;
}

dstd::SharedPtr<comm::backend::Messaging> detail::Hooks::messaging(
    const Communicator& comm) noexcept {
    return comm.backend_ ? comm.backend_->messaging
                         : dstd::SharedPtr<comm::backend::Messaging>{};
}

dstd::SharedPtr<comm::backend::Collectives> detail::Hooks::collectives(
    const Communicator& comm) noexcept {
    return comm.backend_ ? comm.backend_->collectives
                         : dstd::SharedPtr<comm::backend::Collectives>{};
}

dstd::SharedPtr<comm::backend::Bulk> detail::Hooks::bulk(
    const Communicator& comm) noexcept {
    return comm.backend_ ? comm.backend_->bulk
                         : dstd::SharedPtr<comm::backend::Bulk>{};
}

void detail::Hooks::set_bulk(
    Communicator& comm, dstd::SharedPtr<comm::backend::Bulk> bulk) noexcept {
    if (!comm.backend_) return;
    comm.backend_->bulk = std::move(bulk);
}

void detail::Hooks::set_collectives(
    Communicator& comm,
    dstd::SharedPtr<comm::backend::Collectives> collectives) noexcept {
    if (!comm.backend_) return;
    comm.backend_->collectives = std::move(collectives);
}

void detail::Hooks::set_chunk_limit(Communicator& comm,
                                    std::uint64_t bytes) noexcept {
    if (comm.backend_) comm.backend_->collectives->limit_chunk_bytes(bytes);
}

Op<Gathered> detail::Hooks::all_to_all_raw(Group& group,
                                           backend::Contribution contribution,
                                           const CallOpts& opts) {
    detail::DataCall call =
        detail::make_data_call(detail::CallKind::AllToAll, 0, 1, group.size(),
                               backend::RECORD_WANTS | backend::RECORD_PERSONAL,
                               std::move(contribution));
    call.record.color = detail::ALL_TO_ALL_BLOCKS;
    return group.table_call(std::move(call), opts);
}

void detail::Hooks::release_groups(Communicator& comm) noexcept {
    if (!comm.backend_) return;
    dstd::Vector<std::uint64_t> ids;
    if (comm.world_) ids.push_back(comm.world_->id);
    for (const Group& group : comm.node_) {
        const bool known =
            std::find(ids.begin(), ids.end(), group.id()) != ids.end();
        if (!known) ids.push_back(group.id());
    }
    for (const std::uint64_t id : ids) comm.backend_->collectives->release(id);
}

void detail::Hooks::peer_abort(Communicator& comm, std::uint64_t id,
                               std::uint64_t parent, const dstd::Error& why) {
    if (comm.backend_) detail::peer_abort(comm.backend_, id, parent, why);
}

std::uint64_t detail::Hooks::next_group_id(const Communicator& comm) noexcept {
    return comm.backend_
               ? comm.backend_->next_group_id.load(std::memory_order_relaxed)
               : 0;
}

std::size_t detail::Hooks::early_abort_count(const Communicator& comm) {
    if (!comm.backend_) return 0;
    std::lock_guard<std::mutex> lock(comm.backend_->groups_mutex);
    return comm.backend_->early_aborts.size();
}

std::size_t detail::Hooks::live_ops() noexcept {
    return detail::count_live_ops();
}

std::size_t detail::Hooks::live_groups() noexcept {
    return detail::count_live_groups();
}

void detail::Hooks::set_expire_gate(void (*gate)()) noexcept {
    detail::g_expire_gate.store(gate, std::memory_order_release);
}

void detail::Hooks::reset_expire_signal() noexcept {
    detail::g_expire_at_lock.store(false, std::memory_order_release);
    detail::g_expire_returned.store(false, std::memory_order_release);
}

bool detail::Hooks::expire_at_lock() noexcept {
    return detail::g_expire_at_lock.load(std::memory_order_acquire);
}

bool detail::Hooks::expire_returned() noexcept {
    return detail::g_expire_returned.load(std::memory_order_acquire);
}

}  // namespace dftracer::utils::comm
