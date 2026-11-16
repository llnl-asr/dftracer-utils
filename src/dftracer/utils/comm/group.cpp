// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/group.h>
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
    backend->messaging->release(id);
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
    }
    return "unknown";
}

dstd::SharedPtr<GroupState> make_group_state(
    const dstd::SharedPtr<Backend>& backend,
    dstd::Vector<std::uint32_t> members) {
    auto state = dstd::make_shared<GroupState>();
    state->backend = backend;
    state->id = backend->next_group_id.fetch_add(1, std::memory_order_relaxed);
    const std::size_t size = members.size();
    state->next_seq.resize(size, 0);
    state->default_timeout_ns.resize(size, 0);
    state->members = std::move(members);
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

/// `1, 3` for the members that had not arrived at a slot, `none` for all, and
/// why this rank cannot tell.
dstd::String missing_ranks(const backend::Arrival& arrived) {
    if (arrived.empty()) return dstd::String("not known to this rank");
    dstd::Vector<std::uint32_t> missing;
    for (std::uint32_t rank = 0; rank < arrived.size(); ++rank)
        if (!arrived[rank]) missing.push_back(rank);
    if (missing.empty()) return dstd::String("none");
    dstd::String text;
    dstd::fmt_append_join<", ">(text, missing);
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
                  AbortTail& tail) {
    if (!tail.why) return;
    // The parked calls of the channels of the group end with the collectives.
    backend->messaging->abort(id, *tail.why);
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
        if (names_missing) {
            why = Error::from_format<
                "{} at seq {}: deadline {} passed; missing ranks: {}">(
                Condition::Timeout, call_name(kind), seq, deadline_ns,
                missing_ranks(arrived));
        } else {
            why = Error::from_format<"{} at seq {}: deadline {} passed">(
                Condition::Timeout, call_name(kind), seq, deadline_ns);
        }
        abort_locked(*group, abort_error(*group, rank, why), tail);
    }
    finish_abort(group->backend, group->id, tail);
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
    finish_abort(group->backend, group->id, tail);
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
    if constexpr (std::is_void_v<T>) {
        for (std::uint32_t member = 0; member < size; ++member)
            static_cast<OpState<T>*>(slot.ops[member].get())->succeed();
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
        while (begin < colored.size()) {
            std::size_t stop = begin + 1;
            while (stop < colored.size() &&
                   colored[stop].color == colored[begin].color) {
                ++stop;
            }
            dstd::Vector<std::uint32_t> members;
            for (std::size_t at = begin; at < stop; ++at)
                members.push_back(group.members[colored[at].rank]);
            dstd::SharedPtr<GroupState> child =
                make_group_state(group.backend, std::move(members));
            for (std::size_t at = begin; at < stop; ++at) {
                auto* op = static_cast<OpState<Group>*>(
                    slot.ops[colored[at].rank].get());
                op->succeed(Group{group.backend, child,
                                  static_cast<std::uint32_t>(at - begin)});
            }
            begin = stop;
        }
        for (std::uint32_t member = 0; member < size; ++member) {
            if (end.records[member].color != Group::NO_COLOR) continue;
            static_cast<OpState<Group>*>(slot.ops[member].get())
                ->succeed(Group{});
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
                Finisher* finisher, backend::Combine* fold) {
    if (opts.stop.stop_requested()) (void)cancel_group(*group, rank);

    dstd::SharedPtr<Slot> slot;
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        if (group->aborted) {
            state->fail(*group->aborted);
            return;
        }
        seq = group->next_seq[rank]++;
        std::uint64_t deadline = opts.deadline_ns;
        if (deadline == 0 && group->default_timeout_ns[rank] != 0)
            deadline = dstd::steady_now_ns() + group->default_timeout_ns[rank];
        coro::TimerService* timers = backend->bootstrap->timers();
        if (deadline != 0 && timers != nullptr) {
            const std::uint64_t now = dstd::steady_now_ns();
            const std::chrono::nanoseconds delay{deadline > now ? deadline - now
                                                                : 0};
            auto target = dstd::make_shared<detail::TimerTarget>();
            target->group = group;
            target->op = state;
            const coro::TimerService::TimerId timer = timers->register_timeout(
                delay, [target, rank, kind, seq, deadline]() {
                    expire(target, rank, kind, seq, deadline);
                });
            state->set_timeout(timers, timer, std::move(target));
        }

        auto& holder = group->slots[seq];
        if (!holder) {
            // The group of a slot is written once, under this mutex, so a
            // completion reads it without the mutex with no other order.
            holder = dstd::make_shared<Slot>(seq, group->size());
            holder->group = group;
        }
        slot = holder;
        slot->ops[rank] = state;
        if (finisher != nullptr) {
            if (slot->finishers.empty()) {
                slot->finishers.resize(group->size());
                slot->folds.resize(group->size());
            }
            slot->finishers[rank] = std::move(*finisher);
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
               opts, state, &finish_call<T>, nullptr, nullptr);
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
    const bool has_table = !end.table.offsets.empty();
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
        if (kind != CallKind::Scatter && other.count != base.count) {
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
    }
    who = base.root;
    if (kind != CallKind::AllGather && kind != CallKind::AllReduce &&
        base.root >= size) {
        why = Error::from_format<
            "seq {}: root {} is not below the group size {}">(
            Condition::InvalidArgument, slot.seq, base.root, size);
        return false;
    }
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
                     const dstd::Vector<backend::Combine>& folds) {
    Outcome outcome;
    outcome.table = end.table;
    const backend::Record& base = end.records[0];
    if (base.reduce == REDUCE_NONE ||
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
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        if (accept_slot(slot, end, *group, tail)) {
            std::uint32_t who = 0;
            Error why;
            if (check_shape(*group, slot, end, who, why)) {
                finishers = std::move(slot.finishers);
                folds = std::move(slot.folds);
                claimed = true;
                const auto at = group->slots.find(slot.seq);
                if (at != group->slots.end() && at->second.get() == &slot)
                    group->slots.erase(at);
            } else {
                abort_locked(*group, abort_error(*group, who, why), tail);
            }
        }
    }
    finish_abort(group->backend, group->id, tail);
    if (!claimed) return;
    const Outcome outcome = make_outcome(end, folds);
    for (Finisher& finisher : finishers) finisher(outcome);
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
    finish_abort(group.backend, group.id, tail);
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
    finish_abort(group.backend, group.id, tail);
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
                       &detail::finish_data, &finisher, &call.fold);
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
    detail::finish_abort(state_->backend, state_->id, tail);
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
            backend->bootstrap->node_members();
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

dstd::Result<Communicator> Communicator::start_local(std::uint32_t ranks,
                                                     coro::Executor& executor) {
    if (ranks == 0) {
        return dstd::Unexpected(
            dstd::Error::from_format<"a communicator needs at least one rank">(
                Condition::InvalidArgument));
    }
    auto made = dstd::make_shared<detail::Backend>();
    made->bootstrap = backend::make_local_bootstrap(ranks, executor);
    made->messaging = backend::make_local_messaging();
    made->collectives = backend::make_local_collectives();
    made->protocol_version.resize(ranks, detail::PROTOCOL_VERSION);
    dstd::Vector<std::uint32_t> members;
    members.reserve(ranks);
    for (std::uint32_t rank = 0; rank < ranks; ++rank) members.push_back(rank);
    Communicator comm{made};
    comm.world_ = detail::make_group_state(made, std::move(members));
    comm.node_ = make_node_groups(made, comm.world_);
    return comm;
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

void detail::Hooks::set_collectives(
    Communicator& comm,
    dstd::SharedPtr<comm::backend::Collectives> collectives) noexcept {
    if (!comm.backend_) return;
    comm.backend_->collectives = std::move(collectives);
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
