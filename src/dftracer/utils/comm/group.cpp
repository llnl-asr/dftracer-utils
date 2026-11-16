// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
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

std::uint64_t steady_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// One error for every member of the group, naming the rank that caused it.
/// The inner text is copied because an @ref Error holds one block; building
/// the whole message here keeps the one prefix in one place.
Error abort_error(const GroupState& group, std::uint32_t aborter,
                  const Error& why) {
    return Error::from_format<"group {}: aborted by rank {}: {}">(
        why.condition(), group.id, aborter, why.message());
}

/// `1, 3` for the members that had not arrived at @p slot, `none` for all.
dstd::String missing_ranks(const Slot& slot, std::uint32_t size) {
    dstd::Vector<std::uint32_t> missing;
    for (std::uint32_t rank = 0; rank < size; ++rank)
        if (!slot.has_arrived[rank]) missing.push_back(rank);
    if (missing.empty()) return dstd::String("none");
    dstd::String text;
    dstd::fmt_append_join<", ">(text, missing);
    return text;
}

/// Fails every pending call of @p group and refuses the later ones. The first
/// abort wins. Called with the group mutex held, and completes the calls under
/// it: a waiter resuming through its executor may call back and wait here.
void abort_locked(GroupState& group, Error why) {
    if (group.aborted) return;
    group.aborted = std::move(why);
    const Error& shared = *group.aborted;
    auto slots = std::move(group.slots);
    group.slots = decltype(group.slots){};
    for (auto& entry : slots) {
        for (auto& op : entry.second->ops) {
            if (op) op->fail(shared);
        }
    }
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
    if (void (*gate)() = g_expire_gate.load(std::memory_order_acquire)) gate();
    g_expire_at_lock.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(group->mutex);
    // Read under the group mutex: a call that ended in between keeps its
    // outcome, and does not abort its peers.
    if (!op || op->is_done() || group->aborted) return;
    Error why;
    if (kind == CallKind::MonitoredBarrier) {
        const auto slot = group->slots.find(seq);
        const dstd::String missing =
            slot != group->slots.end()
                ? missing_ranks(*slot->second, group->size())
                : dstd::String("none");
        why = Error::from_format<
            "{} at seq {}: deadline {} passed; missing ranks: {}">(
            Condition::Timeout, call_name(kind), seq, deadline_ns, missing);
    } else {
        why = Error::from_format<"{} at seq {}: deadline {} passed">(
            Condition::Timeout, call_name(kind), seq, deadline_ns);
    }
    abort_locked(*group, abort_error(*group, rank, why));
}

/// The handshake: the members of a group must speak one protocol version.
bool check_handshake(const GroupState& group, const Slot& slot, Error& why) {
    const std::uint32_t size = group.size();
    for (std::uint32_t rank = 1; rank < size; ++rank) {
        if (slot.version[rank] == slot.version[0]) continue;
        why = Error::from_format<
            "group {}: rank {} speaks protocol {} but rank {} speaks {}">(
            Condition::Unsupported, group.id, 0, slot.version[0], rank,
            slot.version[rank]);
        return false;
    }
    return true;
}

/**
 * Completes every call of a full @p slot, or aborts the group when their
 * handshake fails. @p T is the value the call gives, which the kind fixes.
 */
template <class T>
void finish_slot(GroupState& group, Slot& slot) {
    Error why;
    if (!check_handshake(group, slot, why)) {
        abort_locked(group, std::move(why));
        return;
    }
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
            if (slot.color[member] == Group::NO_COLOR) continue;
            colored.push_back(
                Colored{member, slot.color[member], slot.key[member]});
        }
        std::sort(colored.begin(), colored.end(),
                  [](const Colored& a, const Colored& b) {
                      if (a.color != b.color) return a.color < b.color;
                      if (a.key != b.key) return a.key < b.key;
                      return a.rank < b.rank;
                  });
        std::size_t begin = 0;
        while (begin < colored.size()) {
            std::size_t end = begin + 1;
            while (end < colored.size() &&
                   colored[end].color == colored[begin].color) {
                ++end;
            }
            dstd::Vector<std::uint32_t> members;
            for (std::size_t at = begin; at < end; ++at)
                members.push_back(group.members[colored[at].rank]);
            dstd::SharedPtr<GroupState> child =
                make_group_state(group.backend, std::move(members));
            for (std::size_t at = begin; at < end; ++at) {
                auto* op = static_cast<OpState<Group>*>(
                    slot.ops[colored[at].rank].get());
                op->succeed(Group{group.backend, child,
                                  static_cast<std::uint32_t>(at - begin)});
            }
            begin = end;
        }
        for (std::uint32_t member = 0; member < size; ++member) {
            if (slot.color[member] != Group::NO_COLOR) continue;
            static_cast<OpState<Group>*>(slot.ops[member].get())
                ->succeed(Group{});
        }
    }
}

/**
 * Records the call of @p rank and completes its slot once every member
 * arrived. The call is registered before anything can fail it, so an abort
 * reaches it like the calls of the peers.
 */
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
    if (opts.stop.stop_requested()) (void)cancel_group(*group, rank);

    {
        std::lock_guard<std::mutex> lock(group->mutex);
        if (group->aborted) {
            state->fail(*group->aborted);
            return Op<T>(std::move(state), group, rank);
        }
        const std::uint64_t seq = group->next_seq[rank]++;
        std::uint64_t deadline = opts.deadline_ns;
        if (deadline == 0 && group->default_timeout_ns[rank] != 0)
            deadline = steady_now_ns() + group->default_timeout_ns[rank];
        if (deadline != 0 && backend->timers != nullptr) {
            const std::uint64_t now = steady_now_ns();
            const std::chrono::nanoseconds delay{deadline > now ? deadline - now
                                                                : 0};
            auto target = dstd::make_shared<detail::TimerTarget>();
            target->group = group;
            target->op = state;
            const coro::TimerService::TimerId timer =
                backend->timers->register_timeout(
                    delay, [target, rank, kind, seq, deadline]() {
                        expire(target, rank, kind, seq, deadline);
                    });
            state->set_timeout(backend->timers, timer, std::move(target));
        }

        auto& holder = group->slots[seq];
        if (!holder) {
            holder = dstd::make_shared<Slot>(group->size());
            holder->kind = kind;
            holder->first_rank = rank;
        }
        const dstd::SharedPtr<Slot> slot = holder;
        slot->ops[rank] = state;
        slot->has_arrived[rank] = 1;
        slot->color[rank] = color;
        slot->key[rank] = key;
        slot->version[rank] = backend->protocol_version[group->members[rank]];
        ++slot->arrived;

        if (slot->kind != kind) {
            const Error inner = Error::from_format<
                "seq {}: rank {} called {} but rank {} called {}">(
                Condition::InvalidArgument, seq, slot->first_rank,
                call_name(slot->kind), rank, call_name(kind));
            abort_locked(*group, abort_error(*group, rank, inner));
        } else if (slot->arrived == group->size()) {
            finish_slot<T>(*group, *slot);
            const auto at = group->slots.find(seq);
            if (at != group->slots.end() && at->second == slot)
                group->slots.erase(at);
        }
    }
    return Op<T>(std::move(state), group, rank);
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
    std::lock_guard<std::mutex> lock(group.mutex);
    abort_locked(group, abort_error(group, rank, canceled_call()));
    return group.aborted ? *group.aborted : canceled_call();
}

void cancel_group(GroupState& group, const PendingOp* op,
                  std::uint32_t rank) noexcept {
    std::lock_guard<std::mutex> lock(group.mutex);
    // Read under the group mutex: a call that ended in between keeps its
    // outcome and does not abort its peers.
    if (op != nullptr && op->is_done()) return;
    abort_locked(group, abort_error(group, rank, canceled_call()));
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

void Group::set_default_deadline(std::uint64_t timeout_ns) noexcept {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->default_timeout_ns[rank_] = timeout_ns;
}

void Group::abort(Error why) noexcept {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    detail::abort_locked(*state_, detail::abort_error(*state_, rank_, why));
}

dstd::Result<Communicator> Communicator::start_local(std::uint32_t ranks,
                                                     coro::Executor& executor) {
    if (ranks == 0) {
        return dstd::Unexpected(
            dstd::Error::from_format<"a communicator needs at least one rank">(
                Condition::InvalidArgument));
    }
    auto backend = dstd::make_shared<detail::Backend>();
    backend->ranks = ranks;
    backend->executor = &executor;
    backend->timers = executor.timers() != nullptr ? executor.timers()
                                                   : &coro::process_timers();
    backend->protocol_version.resize(ranks, detail::PROTOCOL_VERSION);
    dstd::Vector<std::uint32_t> members;
    members.reserve(ranks);
    for (std::uint32_t rank = 0; rank < ranks; ++rank) members.push_back(rank);
    Communicator comm{backend};
    comm.world_ = detail::make_group_state(backend, std::move(members));
    return comm;
}

Group Communicator::world(std::uint32_t rank) noexcept {
    if (rank >= backend_->ranks)
        dstd::fatal("comm: rank is not below the size of the communicator");
    return Group{backend_, world_, rank};
}

Group Communicator::local(std::uint32_t rank) noexcept { return world(rank); }

std::uint32_t Communicator::size() const noexcept { return backend_->ranks; }

void detail::Hooks::set_protocol_version(Communicator& comm, std::uint32_t rank,
                                         std::uint32_t version) noexcept {
    if (!comm.backend_ || rank >= comm.backend_->ranks) return;
    comm.backend_->protocol_version[rank] = version;
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
