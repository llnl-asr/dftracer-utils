// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::backend {

namespace {

class LocalBootstrap : public Bootstrap {
   public:
    LocalBootstrap(std::uint32_t ranks, coro::Executor& executor)
        : ranks_(ranks),
          timers_(executor.timers() != nullptr ? executor.timers()
                                               : &coro::process_timers()) {}

    std::uint32_t ranks() const noexcept override { return ranks_; }

    dstd::Vector<std::uint32_t> node_members() const override {
        dstd::Vector<std::uint32_t> members(ranks_);
        for (std::uint32_t rank = 0; rank < ranks_; ++rank)
            members[rank] = rank;
        return members;
    }

    coro::TimerService* timers() const noexcept override { return timers_; }

   private:
    std::uint32_t ranks_;
    coro::TimerService* timers_;
};

class LocalMessaging : public Messaging {
   public:
    void send(const MessageKey& key, dstd::Vector<std::byte> bytes,
              SendDone done) override {
        dstd::SharedPtr<const dstd::Error> ended;
        RecvDone taker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ended = aborted(key.group);
            if (!ended) {
                Stream& stream = streams_[key];
                if (stream.recvs.empty()) {
                    stream.sends.push_back(
                        Sent{std::move(bytes), std::move(done)});
                    return;
                }
                taker = std::move(stream.recvs.front());
                stream.recvs.pop_front();
                // Erased under the same lock, so no empty stream stays behind.
                if (stream.sends.empty() && stream.recvs.empty())
                    streams_.erase(key);
            }
        }
        if (ended) {
            done(ended.get());
            return;
        }
        taker(dstd::Span<const std::byte>(bytes.data(), bytes.size()), nullptr);
        done(nullptr);
    }

    void recv(const MessageKey& key, RecvDone done) override {
        dstd::SharedPtr<const dstd::Error> ended;
        Sent taken;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ended = aborted(key.group);
            if (!ended) {
                Stream& stream = streams_[key];
                if (stream.sends.empty()) {
                    stream.recvs.push_back(std::move(done));
                    return;
                }
                taken = std::move(stream.sends.front());
                stream.sends.pop_front();
                // Erased under the same lock, so no empty stream stays behind.
                if (stream.sends.empty() && stream.recvs.empty())
                    streams_.erase(key);
            }
        }
        if (ended) {
            done(dstd::Span<const std::byte>(), ended.get());
            return;
        }
        done(
            dstd::Span<const std::byte>(taken.bytes.data(), taken.bytes.size()),
            nullptr);
        taken.done(nullptr);
    }

    void abort(std::uint64_t group, dstd::Error why) override {
        end_group(group, std::move(why), End::Abort);
    }

    void release(std::uint64_t group) override {
        end_group(group,
                  dstd::Error{dstd::Condition::Cancelled, "the group ended"},
                  End::Release);
    }

    LocalCounts counts() const {
        LocalCounts out;
        std::lock_guard<std::mutex> lock(mutex_);
        out.streams = streams_.size();
        out.ended = aborted_.size();
        for (const auto& entry : streams_)
            out.parked += entry.second.sends.size() + entry.second.recvs.size();
        return out;
    }

   private:
    /// Why a group ends: an abort keeps its error for later messages, a release
    /// forgets the group once nothing of it is left.
    enum class End : std::uint8_t { Abort, Release };

    struct Sent {
        dstd::Vector<std::byte> bytes;
        SendDone done;
    };

    struct Stream {
        dstd::Deque<Sent> sends;
        dstd::Deque<RecvDone> recvs;
    };

    /// Ends @p group with @p why: every message of it still parked fails at
    /// once. An abort also makes a later message of the group fail at once
    /// instead of parking, and a release of a group that aborted keeps that
    /// error for the parked messages before it forgets the group.
    void end_group(std::uint64_t group, dstd::Error why, End how) {
        dstd::Vector<Sent> sends;
        dstd::Vector<RecvDone> recvs;
        auto ended = dstd::make_shared<const dstd::Error>(std::move(why));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto at = aborted_.find(group);
            if (how == End::Abort) {
                if (at == aborted_.end()) {
                    aborted_.emplace(group, ended);
                } else {
                    at->second = ended;
                }
            } else {
                if (at != aborted_.end()) {
                    ended = at->second;
                    aborted_.erase(at);
                }
            }
            take(group, sends, recvs);
        }
        // Ended outside the lock: a completion may send or receive again.
        for (const RecvDone& recv : recvs)
            recv(dstd::Span<const std::byte>(), ended.get());
        for (const Sent& sent : sends) sent.done(ended.get());
    }

    /// The error @p group ended with, or nothing. Called under the mutex.
    dstd::SharedPtr<const dstd::Error> aborted(std::uint64_t group) const {
        auto at = aborted_.find(group);
        return at == aborted_.end() ? dstd::SharedPtr<const dstd::Error>{}
                                    : at->second;
    }

    /// Takes what @p group still holds, sending and receiving its streams.
    /// Called under the mutex.
    void take(std::uint64_t group, dstd::Vector<Sent>& sends,
              dstd::Vector<RecvDone>& recvs) {
        for (auto at = streams_.begin(); at != streams_.end();) {
            if (at->first.group != group) {
                ++at;
                continue;
            }
            for (Sent& sent : at->second.sends)
                sends.push_back(std::move(sent));
            for (RecvDone& recv : at->second.recvs)
                recvs.push_back(std::move(recv));
            at = streams_.erase(at);
        }
    }

    mutable std::mutex mutex_;
    dstd::Map<MessageKey, Stream> streams_;
    /// One error per group that ended with one, so a later message of that
    /// group fails at once instead of parking.
    dstd::Map<std::uint64_t, dstd::SharedPtr<const dstd::Error>> aborted_;
};

/// One slot of the local collectives: a group id and a sequence number.
struct SlotKey {
    std::uint64_t group = 0;
    std::uint64_t seq = 0;

    friend bool operator<(const SlotKey& a, const SlotKey& b) noexcept {
        if (a.group != b.group) return a.group < b.group;
        return a.seq < b.seq;
    }
};

/// The calls the members made at one slot. A local provider sees every member,
/// so one slot holds the call of every rank that arrived.
struct LocalSlot {
    LocalSlot(std::uint32_t members, Collectives::Done done)
        : records(members),
          seen(members, 0),
          members(members),
          done(std::move(done)) {}

    dstd::Vector<Record> records;
    dstd::Vector<std::uint8_t> seen;
    std::uint32_t members = 0;
    std::uint32_t arrived = 0;
    /// The rank of the first member that arrived. Its call is the base of the
    /// comparison, so every other arrival is compared against the same record.
    std::uint32_t anchor = 0;
    Collectives::Done done;
};

class LocalCollectives : public Collectives {
   public:
    bool arrive(std::uint64_t group_id, std::uint64_t seq,
                std::uint32_t members, std::uint32_t rank, const Record& record,
                Done done) override {
        dstd::Optional<LocalSlot> ended_slot;
        bool broken = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const SlotKey key{group_id, seq};
            auto at = slots_.find(key);
            if (at == slots_.end())
                at = slots_.try_emplace(key, members, std::move(done)).first;
            LocalSlot& slot = at->second;
            const bool opens = slot.arrived == 0;
            slot.records[rank] = record;
            slot.seen[rank] = 1;
            if (opens) slot.anchor = rank;
            // Broken as soon as a member names another call than the first
            // arrival, so the rank that disagrees never waits for a member
            // that is not coming.
            broken = !opens && slot.records[slot.anchor].kind != record.kind;
            ++slot.arrived;
            if (!broken && slot.arrived < slot.members) return true;
            ended_slot.emplace(std::move(slot));
            slots_.erase(at);
        }
        const LocalSlot& slot = *ended_slot;
        const SlotEnd end{
            dstd::Span<const Record>(slot.records.data(), slot.records.size()),
            broken ? dstd::Span<const std::uint8_t>(slot.seen.data(),
                                                    slot.seen.size())
                   : dstd::Span<const std::uint8_t>(),
            rank, nullptr};
        slot.done(end);
        return false;
    }

    void drop(std::uint64_t group_id, std::uint64_t seq,
              Arrival* arrived) override {
        dstd::Optional<LocalSlot> gone;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto at = slots_.find(SlotKey{group_id, seq});
            if (at == slots_.end()) return;
            gone.emplace(std::move(at->second));
            slots_.erase(at);
            if (arrived != nullptr) *arrived = gone->seen;
        }
    }

    void release(std::uint64_t group_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto at = slots_.begin(); at != slots_.end();) {
            if (at->first.group == group_id) {
                at = slots_.erase(at);
                continue;
            }
            ++at;
        }
    }

   private:
    std::mutex mutex_;
    dstd::Map<SlotKey, LocalSlot> slots_;
};

}  // namespace

dstd::SharedPtr<Bootstrap> make_local_bootstrap(std::uint32_t ranks,
                                                coro::Executor& executor) {
    return dstd::make_shared<LocalBootstrap>(ranks, executor);
}

dstd::SharedPtr<Messaging> make_local_messaging() {
    return dstd::make_shared<LocalMessaging>();
}

dstd::SharedPtr<Collectives> make_local_collectives() {
    return dstd::make_shared<LocalCollectives>();
}

LocalCounts local_counts(const Messaging& messaging) {
    const auto* local = dynamic_cast<const LocalMessaging*>(&messaging);
    if (local == nullptr) dstd::fatal("only a local messaging counts");
    return local->counts();
}

}  // namespace dftracer::utils::comm::backend
