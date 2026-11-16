// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::backend {

namespace {

/// The two messages of the linear protocol: a member's record to group rank 0,
/// and the table of every record back from it. Each call of a group has its own
/// two streams, so a message of one call never answers the receive of another.
constexpr std::uint64_t PURPOSE_RECORD = 0;
constexpr std::uint64_t PURPOSE_TABLE = 1;

std::uint64_t message_tag(std::uint64_t seq, std::uint64_t purpose) noexcept {
    return (seq << 1) | purpose;
}

// The bytes of a message belong to the message, so both directions copy the
// records: one copy of 16 bytes per record and per message.
dstd::Vector<std::byte> pack_record(const Record& record) {
    dstd::Vector<std::byte> bytes(sizeof(Record));
    std::memcpy(bytes.data(), &record, sizeof(Record));
    return bytes;
}

dstd::Vector<std::byte> pack_table(const dstd::Vector<Record>& records) {
    dstd::Vector<std::byte> bytes(records.size() * sizeof(Record));
    std::memcpy(bytes.data(), records.data(), bytes.size());
    return bytes;
}

void unpack_record(dstd::Span<const std::byte> bytes, Record& out) {
    if (bytes.size() != sizeof(Record)) {
        dstd::fatal("a collective record has the wrong size");
    }
    std::memcpy(&out, bytes.data(), sizeof(Record));
}

/// One slot of a group: a group id and a sequence number.
struct SlotKey {
    std::uint64_t group = 0;
    std::uint64_t seq = 0;

    friend bool operator<(const SlotKey& a, const SlotKey& b) noexcept {
        if (a.group != b.group) return a.group < b.group;
        return a.seq < b.seq;
    }
};

/// What the provider knows of one slot.
struct Slot {
    Slot(const SlotKey& key, std::uint32_t members)
        : key(key), members(members), records(members), seen(members, 0) {}

    SlotKey key;
    std::uint32_t members = 0;
    /// Members that arrived, by their record or by their own arrival; each
    /// counts once.
    std::uint32_t arrived = 0;
    /// Rank 0 of the group arrived in this process, so this provider collects
    /// the slot and can see every member.
    bool collecting = false;
    bool ended = false;
    /// The member of the first arrival. Its call is the base of the comparison,
    /// so every later arrival is compared against the same record.
    std::uint32_t anchor = 0;
    dstd::Vector<Record> records;
    /// One byte per member, 1 for a member that arrived.
    dstd::Vector<std::uint8_t> seen;
    Collectives::Done done;
};

class LinearCollectives : public Collectives {
   public:
    explicit LinearCollectives(dstd::SharedPtr<Messaging> messaging)
        : messaging_(std::move(messaging)) {}

    /// Fails the messages still parked for a slot that never completed, so none
    /// answers a provider that is gone.
    ~LinearCollectives() override {
        dstd::Vector<std::uint64_t> groups;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : slots_) {
                if (groups.empty() || groups.back() != entry.first.group)
                    groups.push_back(entry.first.group);
            }
        }
        for (const std::uint64_t group : groups)
            messaging_->abort(group, dstd::Error{dstd::Condition::Cancelled,
                                                 "the collectives ended"});
    }

    bool arrive(std::uint64_t group_id, std::uint64_t seq,
                std::uint32_t members, std::uint32_t rank, const Record& record,
                Done done) override {
        dstd::SharedPtr<Slot> slot;
        Arrival broken;
        bool collecting = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const SlotKey key{group_id, seq};
            auto at = slots_.find(key);
            if (at == slots_.end()) {
                slot = dstd::make_shared<Slot>(key, members);
                slots_.emplace(key, slot);
            } else {
                slot = at->second;
            }
            if (slot->ended) return false;
            if (!slot->done) slot->done = std::move(done);
            const bool agrees = arrive_locked(*slot, rank, record);
            if (rank == 0 && !slot->collecting) {
                slot->collecting = true;
                collecting = true;
            }
            // A member that shares this slot with the arrival sees the
            // disagreement here, so it never waits for a member that is not
            // coming. A member that does not is told by its peer's abort.
            if (!agrees) broken = slot->seen;
        }
        if (!broken.empty()) {
            finish_slot(slot, broken, rank, nullptr);
            return false;
        }
        if (rank == 0) {
            if (collecting) collect(slot);
        } else {
            contribute(slot, rank);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return !slot->ended;
    }

    void release(std::uint64_t group_id) override {
        dstd::Vector<dstd::SharedPtr<Slot>> gone;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto at = slots_.begin(); at != slots_.end();) {
                if (at->first.group != group_id) {
                    ++at;
                    continue;
                }
                gone.push_back(at->second);
                at = slots_.erase(at);
            }
            for (const dstd::SharedPtr<Slot>& slot : gone) slot->ended = true;
        }
        messaging_->release(group_id);
    }

    void drop(std::uint64_t group_id, std::uint64_t seq,
              Arrival* arrived) override {
        dstd::SharedPtr<Slot> gone;
        Arrival mask;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto at = slots_.find(SlotKey{group_id, seq});
            if (at == slots_.end()) return;
            gone = at->second;
            gone->ended = true;
            slots_.erase(at);
            // Only a rank 0 of this process saw every member; any other rank
            // would guess, so it reports nothing.
            if (gone->seen[0] != 0) mask = gone->seen;
        }
        if (arrived != nullptr) *arrived = std::move(mask);
        // Outside the lock, since the parked messages answer it here.
        messaging_->abort(group_id, dstd::Error{dstd::Condition::Cancelled,
                                                "the collective ended"});
    }

   private:
    /// Notes the arrival of @p rank with @p record, once, and gives false when
    /// its call names another call than the first arrival of the slot. Called
    /// under the mutex.
    static bool arrive_locked(Slot& slot, std::uint32_t rank,
                              const Record& record) {
        slot.records[rank] = record;
        if (slot.seen[rank] != 0) return true;
        slot.seen[rank] = 1;
        const bool opens = slot.arrived == 0;
        if (opens) slot.anchor = rank;
        ++slot.arrived;
        return opens || slot.records[slot.anchor].kind == record.kind;
    }

    /// Sends the record of @p rank to group rank 0 and asks for the table back.
    void contribute(const dstd::SharedPtr<Slot>& slot, std::uint32_t rank) {
        dstd::Vector<std::byte> bytes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot->ended) return;
            bytes = pack_record(slot->records[rank]);
        }
        const MessageKey key{slot->key.group, rank, 0,
                             message_tag(slot->key.seq, PURPOSE_RECORD)};
        Messaging::SendDone sent = [this, slot, rank](const dstd::Error* why) {
            if (why != nullptr) finish_slot(slot, Arrival{}, rank, why);
        };
        messaging_->send(key, std::move(bytes), std::move(sent));
        take_table(slot, rank);
    }

    void take_table(const dstd::SharedPtr<Slot>& slot, std::uint32_t rank) {
        const MessageKey key{slot->key.group, 0, rank,
                             message_tag(slot->key.seq, PURPOSE_TABLE)};
        Messaging::RecvDone took = [this, slot, rank](
                                       dstd::Span<const std::byte> bytes,
                                       const dstd::Error* why) {
            if (why != nullptr) {
                finish_slot(slot, Arrival{}, rank, why);
                return;
            }
            if (bytes.size() != slot->members * sizeof(Record)) {
                dstd::fatal("a collective table has the wrong size");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Ended under this lock, so another callback already finished
                // the slot and the span its completion reads is gone.
                if (slot->ended) return;
                for (std::uint32_t member = 0; member < slot->members;
                     ++member) {
                    unpack_record(
                        bytes.subspan(member * sizeof(Record), sizeof(Record)),
                        slot->records[member]);
                }
            }
            finish_slot(slot, Arrival{}, rank, nullptr);
        };
        messaging_->recv(key, std::move(took));
    }

    /// Receives the record of every other member, as group rank 0.
    void collect(const dstd::SharedPtr<Slot>& slot) {
        for (std::uint32_t member = 1; member < slot->members; ++member) {
            const MessageKey key{slot->key.group, member, 0,
                                 message_tag(slot->key.seq, PURPOSE_RECORD)};
            Messaging::RecvDone took = [this, slot, member](
                                           dstd::Span<const std::byte> bytes,
                                           const dstd::Error* why) {
                if (why != nullptr) {
                    finish_slot(slot, Arrival{}, 0, why);
                    return;
                }
                bool full = false;
                Arrival broken;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (slot->ended) return;
                    if (bytes.size() != sizeof(Record)) {
                        dstd::fatal("a collective record has the wrong size");
                    }
                    Record record;
                    unpack_record(bytes, record);
                    if (arrive_locked(*slot, member, record))
                        full = slot->arrived == slot->members;
                    else
                        broken = slot->seen;
                }
                if (!broken.empty()) {
                    finish_slot(slot, broken, member, nullptr);
                    return;
                }
                if (full) broadcast(slot);
            };
            messaging_->recv(key, std::move(took));
        }
        if (slot->members == 1) broadcast(slot);
    }

    /// Sends the table of @p slot to every other member, as group rank 0.
    void broadcast(const dstd::SharedPtr<Slot>& slot) {
        dstd::Vector<std::byte> table;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot->ended) return;
            table = pack_table(slot->records);
        }
        // Every member, even after a peer ended the slot here: the receive of a
        // peer that already ended stays parked otherwise. The bytes belong to
        // each message, so each send takes its own copy.
        for (std::uint32_t member = 1; member < slot->members; ++member) {
            dstd::Vector<std::byte> bytes(table);
            const MessageKey key{slot->key.group, 0, member,
                                 message_tag(slot->key.seq, PURPOSE_TABLE)};
            Messaging::SendDone sent = [this, slot,
                                        member](const dstd::Error* why) {
                if (why != nullptr) finish_slot(slot, Arrival{}, member, why);
            };
            messaging_->send(key, std::move(bytes), std::move(sent));
        }
        finish_slot(slot, Arrival{}, 0, nullptr);
    }

    /// Ends @p slot once, with @p seen empty when every member arrived, and
    /// reports it outside the lock so the caller's group lock is free.
    void finish_slot(const dstd::SharedPtr<Slot>& slot, const Arrival& seen,
                     std::uint32_t rank, const dstd::Error* why) {
        Done done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot->ended) return;
            slot->ended = true;
            auto at = slots_.find(slot->key);
            if (at != slots_.end() && at->second == slot) slots_.erase(at);
            done = std::move(slot->done);
        }
        if (!done) return;
        // An empty mask means every member arrived; a table that came without
        // them is a defect of the provider, not a complete slot.
        const Arrival mask =
            seen.empty() && slot->arrived != slot->members ? slot->seen : seen;
        const SlotEnd end{
            dstd::Span<const Record>(slot->records.data(),
                                     slot->records.size()),
            dstd::Span<const std::uint8_t>(mask.data(), mask.size()), rank,
            why};
        done(end);
    }

    mutable std::mutex mutex_;
    dstd::Map<SlotKey, dstd::SharedPtr<Slot>> slots_;
    dstd::SharedPtr<Messaging> messaging_;
};

}  // namespace

dstd::SharedPtr<Collectives> make_linear_collectives(
    dstd::SharedPtr<Messaging> messaging) {
    return dstd::make_shared<LinearCollectives>(std::move(messaging));
}

}  // namespace dftracer::utils::comm::backend
