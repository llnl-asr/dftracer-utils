// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/fault_messaging.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/map.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::backend {

namespace {

constexpr std::uint64_t PARTS = 1'000'000;
constexpr std::uint64_t SALT_DROP = 0x44524F50ull;
constexpr std::uint64_t SALT_DELAY = 0x44454C59ull;
constexpr std::uint64_t SALT_ORDER = 0x4F524452ull;

/// A value that depends on the seed, the stream, the place of the message in it
/// and what the value is for, and on nothing else.
std::uint64_t draw(std::uint64_t seed, const MessageKey& key,
                   std::uint64_t index, std::uint64_t salt) noexcept {
    using dstd::hash::splitmix64;
    std::uint64_t x = splitmix64(seed ^ salt);
    x = splitmix64(x ^ key.group);
    x = splitmix64(x ^ ((std::uint64_t{key.source} << 32) | key.destination));
    x = splitmix64(x ^ key.tag);
    return splitmix64(x ^ index);
}

/// A message or a close that waits in its stream.
struct Entry {
    bool is_close = false;
    bool held = false;
    MessageKey key;
    Payload payload;
    StreamOpts opts;
    Messaging::SendDone done;
    std::uint64_t index = 0;
    std::uint64_t ticket = 0;
    /// On the steady clock.
    std::uint64_t due_ns = 0;
    std::uint64_t held_since_ns = 0;
    /// What decides which stream goes first when several fronts can leave.
    std::uint64_t order = 0;
};

struct Stream {
    dstd::Deque<Entry> queue;
    /// Sends so far, which numbers the next one.
    std::uint64_t sent = 0;
    /// The due time of the last message queued, so a later one never leaves
    /// before it.
    std::uint64_t last_due_ns = 0;
    std::uint32_t held = 0;
    /// What each rule (by its place) dropped from this stream, which its limit
    /// counts: the schedule of a stream then never depends on the timing of the
    /// others.
    dstd::Map<std::size_t, std::uint64_t> drops;
};

struct KillAbort {
    bool any = false;
    dstd::Vector<std::uint64_t> groups;
    dstd::Error why;
};

class Fault final : public FaultMessaging,
                    public std::enable_shared_from_this<Fault> {
   public:
    Fault(dstd::SharedPtr<Messaging> inner, FaultConfig config,
          coro::TimerService* timers)
        : inner_(std::move(inner)),
          config_(std::move(config)),
          timers_(timers) {
        for (const FaultKill& kill : config_.kills)
            kill_after_[kill.rank] = kill.after_ops;
    }

    ~Fault() override {
        coro::TimerService::TimerId timer = coro::TimerService::NO_TIMER;
        dstd::Vector<Entry> left;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timer = timer_;
            for (auto& entry : streams_)
                for (Entry& queued : entry.second.queue)
                    left.push_back(std::move(queued));
            streams_.clear();
        }
        if (timer != coro::TimerService::NO_TIMER && timers_ != nullptr)
            timers_->cancel_timeout(timer);
        const dstd::Error why{dstd::Condition::Cancelled,
                              "the fault provider ended"};
        for (Entry& entry : left)
            if (entry.done) entry.done(&why);
    }

    OpId send(const MessageKey& key, Payload payload, StreamOpts opts,
              SendDone done) override {
        KillAbort kill;
        dstd::Error dead;
        bool is_dead = false;
        bool direct = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            is_dead =
                guard_locked(world_locked(key.group, key.source), dead, kill);
            if (!is_dead) {
                Stream& stream = streams_[key];
                const std::uint64_t index = stream.sent++;
                Decision decision = decide_locked(stream, key, index);
                note_locked(key, index, decision);
                if (decision.action == FaultAction::Drop) {
                    ++counts_.dropped;
                    lock.unlock();
                    done(nullptr);
                    return NO_OP;
                }
                direct = decision.action == FaultAction::Pass &&
                         stream.queue.empty() && !draining_;
                if (direct) {
                    ++counts_.passed;
                } else {
                    enqueue_locked(stream, key, index, decision,
                                   std::move(payload), opts, std::move(done));
                }
            }
        }
        if (is_dead) {
            fire(kill);
            done(&dead);
            return NO_OP;
        }
        if (direct)
            return inner_->send(key, std::move(payload), opts, std::move(done));
        drain();
        return NO_OP;
    }

    OpId recv(const MessageKey& key, RecvDone done) override {
        KillAbort kill;
        dstd::Error dead;
        if (receiver_dead(key, dead, kill)) {
            fire(kill);
            done(dstd::Span<const std::byte>(), &dead);
            return NO_OP;
        }
        return inner_->recv(key, std::move(done));
    }

    OpId recv_any_source(const MessageKey& key, std::uint32_t senders,
                         RecvFromDone done) override {
        KillAbort kill;
        dstd::Error dead;
        if (receiver_dead(key, dead, kill)) {
            fire(kill);
            done(0, dstd::Span<const std::byte>(), &dead);
            return NO_OP;
        }
        return inner_->recv_any_source(key, senders, std::move(done));
    }

    OpId recv_into(const MessageKey& key, std::uint32_t senders,
                   dstd::Span<std::byte> buffer, RecvIntoDone done) override {
        KillAbort kill;
        dstd::Error dead;
        if (receiver_dead(key, dead, kill)) {
            fire(kill);
            done(0, 0, &dead);
            return NO_OP;
        }
        return inner_->recv_into(key, senders, buffer, std::move(done));
    }

    bool cancel(const MessageKey& key, OpId op, dstd::Error why) override {
        if (op.value == 0) return false;
        return inner_->cancel(key, op, std::move(why));
    }

    void close(const MessageKey& key) override {
        bool direct = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (killed_.count(world_locked(key.group, key.source)) != 0) return;
            Stream& stream = streams_[key];
            direct = stream.queue.empty() && !draining_;
            if (!direct) {
                Entry entry;
                entry.is_close = true;
                entry.key = key;
                entry.ticket = next_ticket_++;
                entry.due_ns =
                    std::max(dstd::steady_now_ns(), stream.last_due_ns);
                stream.last_due_ns = entry.due_ns;
                stream.queue.push_back(std::move(entry));
                if (draining_) again_ = true;
            }
        }
        if (direct)
            inner_->close(key);
        else
            drain();
    }

    void abort(std::uint64_t group, dstd::Error why) override {
        take_group(group, why);
        inner_->abort(group, std::move(why));
    }

    void on_peer_abort(PeerAbort handler) override {
        auto shared = dstd::make_shared<PeerAbort>(std::move(handler));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            peer_abort_ = shared;
        }
        inner_->on_peer_abort([shared](std::uint64_t group,
                                       std::uint64_t parent,
                                       const dstd::Error& why) {
            (*shared)(group, parent, why);
        });
    }

    void announce_abort(std::uint64_t group, std::uint64_t parent,
                        const dstd::Error& why) override {
        inner_->announce_abort(group, parent, why);
    }

    void bind_group(std::uint64_t group,
                    dstd::Span<const std::uint32_t> members) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            groups_[group].assign(members.begin(), members.end());
        }
        inner_->bind_group(group, members);
    }

    void release(std::uint64_t group) override {
        take_group(group, dstd::Error{dstd::Condition::Cancelled,
                                      "the group was released"});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            groups_.erase(group);
        }
        inner_->release(group);
    }

    void kill(std::uint32_t rank) override {
        KillAbort kill;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mark_killed_locked(rank, kill);
        }
        fire(kill);
    }

    void flush() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            flush_requested_ = true;
            if (draining_) again_ = true;
        }
        drain();
    }

    FaultCounts counts() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_;
    }

    dstd::Vector<FaultEvent> schedule() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

   private:
    struct Decision {
        FaultAction action = FaultAction::Pass;
        std::uint64_t delay_ns = 0;
        std::uint32_t reorder_hold = 0;
    };

    /// The world rank of group rank @p rank of @p group.
    std::uint32_t world_locked(std::uint64_t group, std::uint32_t rank) const {
        const auto at = groups_.find(group);
        if (at == groups_.end() || rank >= at->second.size()) return rank;
        return at->second[rank];
    }

    bool matches(const FaultMatch& match, const MessageKey& key) const {
        if (match.source != ANY_SOURCE && match.source != key.source)
            return false;
        if (match.destination != ANY_SOURCE &&
            match.destination != key.destination)
            return false;
        return !match.purpose || *match.purpose == tag_purpose(key.tag);
    }

    Decision decide_locked(Stream& stream, const MessageKey& key,
                           std::uint64_t index) {
        Decision decision;
        for (std::size_t at = 0; at < config_.rules.size(); ++at) {
            const FaultRule& rule = config_.rules[at];
            if (!matches(rule.match, key)) continue;
            if (rule.drop_per_million != 0 &&
                (rule.drop_limit == 0 || stream.drops[at] < rule.drop_limit) &&
                draw(config_.seed, key, index, SALT_DROP) % PARTS <
                    rule.drop_per_million) {
                ++stream.drops[at];
                decision.action = FaultAction::Drop;
                return decision;
            }
            if (timers_ == nullptr) return decision;
            if (rule.delay_max_ns != 0) {
                const std::uint64_t low =
                    std::min(rule.delay_min_ns, rule.delay_max_ns);
                const std::uint64_t span = rule.delay_max_ns - low + 1;
                decision.delay_ns =
                    low + draw(config_.seed, key, index, SALT_DELAY) % span;
                decision.action = FaultAction::Delay;
            }
            if (rule.reorder_hold != 0) {
                decision.action = FaultAction::Hold;
                decision.reorder_hold = rule.reorder_hold;
            }
            return decision;
        }
        return decision;
    }

    void note_locked(const MessageKey& key, std::uint64_t index,
                     const Decision& decision) {
        if (decision.action == FaultAction::Delay) ++counts_.delayed;
        if (decision.action == FaultAction::Hold) ++counts_.held;
        if (!config_.record_schedule) return;
        events_.push_back(
            FaultEvent{key, index, decision.action, decision.delay_ns});
    }

    void enqueue_locked(Stream& stream, const MessageKey& key,
                        std::uint64_t index, const Decision& decision,
                        Payload payload, StreamOpts opts, SendDone done) {
        Entry entry;
        entry.key = key;
        entry.payload = std::move(payload);
        entry.opts = opts;
        entry.done = std::move(done);
        entry.index = index;
        entry.ticket = next_ticket_++;
        const std::uint64_t now = dstd::steady_now_ns();
        entry.due_ns = std::max(now + decision.delay_ns, stream.last_due_ns);
        stream.last_due_ns = entry.due_ns;
        entry.order = draw(config_.seed, key, index, SALT_ORDER);
        if (decision.action == FaultAction::Hold) {
            entry.held = true;
            entry.held_since_ns = now;
            ++stream.held;
            ++held_count_;
            if (stream.held >= decision.reorder_hold) flush_requested_ = true;
        }
        stream.queue.push_back(std::move(entry));
        if (draining_) again_ = true;
    }

    /// Counts a send or receive of @p world and says whether that rank is dead.
    /// A rank that dies here sets @p kill.
    bool guard_locked(std::uint32_t world, dstd::Error& why, KillAbort& kill) {
        if (killed_.count(world) == 0) {
            const auto at = kill_after_.find(world);
            std::uint64_t& ops = ops_[world];
            if (at == kill_after_.end() || ops < at->second) {
                ++ops;
                return false;
            }
            mark_killed_locked(world, kill);
        }
        ++counts_.killed_ops;
        why = killed_error(world);
        return true;
    }

    static dstd::Error killed_error(std::uint32_t rank) {
        return dstd::Error::from_format<
            "rank {} was killed by fault injection">(dstd::Condition::Io, rank);
    }

    bool receiver_dead(const MessageKey& key, dstd::Error& why,
                       KillAbort& kill) {
        std::lock_guard<std::mutex> lock(mutex_);
        return guard_locked(world_locked(key.group, key.destination), why,
                            kill);
    }

    /// Marks @p rank dead, once, and names the groups that hold it in @p kill.
    void mark_killed_locked(std::uint32_t rank, KillAbort& kill) {
        if (!killed_.insert(rank).second) return;
        kill.any = true;
        kill.why = killed_error(rank);
        for (const auto& entry : groups_) {
            const auto& members = entry.second;
            if (std::find(members.begin(), members.end(), rank) !=
                members.end())
                kill.groups.push_back(entry.first);
        }
    }

    /// Aborts the groups of a kill from the timer thread, never under a lock of
    /// the caller.
    void fire(KillAbort& kill) {
        if (!kill.any) return;
        if (timers_ == nullptr) {
            abort_groups(kill.groups, kill.why);
            return;
        }
        std::weak_ptr<Fault> weak = weak_from_this();
        (void)timers_->register_timeout(
            std::chrono::nanoseconds{0},
            [weak, groups = std::move(kill.groups), why = kill.why]() {
                if (const auto self = weak.lock())
                    self->abort_groups(groups, why);
            });
    }

    void abort_groups(const dstd::Vector<std::uint64_t>& groups,
                      const dstd::Error& why) {
        dstd::SharedPtr<PeerAbort> handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = peer_abort_;
        }
        if (!handler) return;
        for (const std::uint64_t group : groups) (*handler)(group, 0, why);
    }

    /// Ends the messages that wait for @p group with @p why.
    void take_group(std::uint64_t group, const dstd::Error& why) {
        dstd::Vector<Entry> gone;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto at = streams_.lower_bound(MessageKey{group, 0, 0, 0});
                 at != streams_.end() && at->first.group == group;) {
                for (Entry& entry : at->second.queue) {
                    if (entry.held) --held_count_;
                    gone.push_back(std::move(entry));
                }
                at = streams_.erase(at);
            }
        }
        for (Entry& entry : gone)
            if (entry.done) entry.done(&why);
    }

    /// Moves the messages that may leave to the provider below, in order. One
    /// thread does it at a time, so the order of one stream is never broken.
    void drain() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (draining_) {
                again_ = true;
                return;
            }
            draining_ = true;
        }
        std::uint64_t wake_ns = 0;
        bool has_wake = false;
        for (;;) {
            dstd::Vector<Entry> out;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                collect_locked(out);
                if (out.empty() && !again_) {
                    draining_ = false;
                    has_wake = next_wake_locked(wake_ns);
                    break;
                }
                again_ = false;
            }
            for (Entry& entry : out) {
                if (entry.is_close)
                    inner_->close(entry.key);
                else
                    (void)inner_->send(entry.key, std::move(entry.payload),
                                       entry.opts, std::move(entry.done));
            }
        }
        if (has_wake) arm(wake_ns);
    }

    void collect_locked(dstd::Vector<Entry>& out) {
        const std::uint64_t now = dstd::steady_now_ns();
        if (held_count_ != 0 && !flush_requested_) {
            for (const auto& entry : streams_)
                for (const Entry& queued : entry.second.queue)
                    if (queued.held &&
                        queued.held_since_ns + config_.reorder_flush_ns <= now)
                        flush_requested_ = true;
        }
        if (flush_requested_) {
            for (auto& entry : streams_) {
                for (Entry& queued : entry.second.queue) queued.held = false;
                entry.second.held = 0;
            }
            held_count_ = 0;
            flush_requested_ = false;
        }
        for (;;) {
            Stream* best = nullptr;
            for (auto& entry : streams_) {
                Stream& stream = entry.second;
                if (stream.queue.empty()) continue;
                const Entry& front = stream.queue.front();
                if (front.held || front.due_ns > now) continue;
                if (best == nullptr ||
                    front.order < best->queue.front().order ||
                    (front.order == best->queue.front().order &&
                     front.ticket < best->queue.front().ticket))
                    best = &stream;
            }
            if (best == nullptr) break;
            Entry& front = best->queue.front();
            if (config_.record_schedule)
                events_.push_back(FaultEvent{front.key, front.index,
                                             FaultAction::Release, 0});
            out.push_back(std::move(front));
            best->queue.pop_front();
        }
    }

    /// When the provider must look again, if a message waits.
    bool next_wake_locked(std::uint64_t& wake_ns) const {
        bool found = false;
        for (const auto& entry : streams_) {
            for (const Entry& queued : entry.second.queue) {
                const std::uint64_t at =
                    queued.held
                        ? queued.held_since_ns + config_.reorder_flush_ns
                        : queued.due_ns;
                if (!found || at < wake_ns) wake_ns = at;
                found = true;
            }
        }
        return found && timers_ != nullptr;
    }

    void arm(std::uint64_t wake_ns) {
        const std::uint64_t now = dstd::steady_now_ns();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (timer_ != coro::TimerService::NO_TIMER &&
                timer_at_ns_ <= wake_ns && timer_at_ns_ > now)
                return;
            timer_at_ns_ = wake_ns;
        }
        std::weak_ptr<Fault> weak = weak_from_this();
        const coro::TimerService::TimerId id = timers_->register_timeout(
            std::chrono::nanoseconds{wake_ns > now ? wake_ns - now : 0},
            [weak]() {
                if (const auto self = weak.lock()) self->drain();
            });
        std::lock_guard<std::mutex> lock(mutex_);
        timer_ = id;
    }

    dstd::SharedPtr<Messaging> inner_;
    FaultConfig config_;
    coro::TimerService* timers_ = nullptr;

    mutable std::mutex mutex_;
    dstd::Map<MessageKey, Stream> streams_;
    dstd::Map<std::uint64_t, dstd::Vector<std::uint32_t>> groups_;
    dstd::Map<std::uint32_t, std::uint64_t> kill_after_;
    dstd::Map<std::uint32_t, std::uint64_t> ops_;
    dstd::Set<std::uint32_t> killed_;
    dstd::SharedPtr<PeerAbort> peer_abort_;
    FaultCounts counts_;
    dstd::Vector<FaultEvent> events_;
    std::uint64_t next_ticket_ = 0;
    std::uint32_t held_count_ = 0;
    bool flush_requested_ = false;
    bool draining_ = false;
    bool again_ = false;
    coro::TimerService::TimerId timer_ = coro::TimerService::NO_TIMER;
    std::uint64_t timer_at_ns_ = 0;
};

}  // namespace

dstd::SharedPtr<FaultMessaging> make_fault_messaging(
    dstd::SharedPtr<Messaging> inner, FaultConfig config,
    coro::TimerService* timers) {
    return dstd::make_shared<Fault>(std::move(inner), std::move(config),
                                    timers);
}

}  // namespace dftracer::utils::comm::backend
