// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/arrival_fold.h>
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

/// The linear protocol sends a member's record to group rank 0, its payload
/// when the call carries a table, and the table of every record and payload
/// back from it. Each call of a group has its own streams, so a message of one
/// call never answers the receive of another, and no channel message meets
/// any. The record and the payload of call `seq` take the indexes `2 * seq`
/// and `2 * seq + 1` of the record purpose.

// The bytes of a record belong to the message, so a record is copied once per
// message; a payload is sent as a borrowed view with no copy.
dstd::Vector<std::byte> pack_record(const Record& record) {
    dstd::Vector<std::byte> bytes(sizeof(Record));
    std::memcpy(bytes.data(), &record, sizeof(Record));
    return bytes;
}

void unpack_record(dstd::Span<const std::byte> bytes, Record& out) {
    if (bytes.size() != sizeof(Record)) {
        dstd::fatal("a collective record has the wrong size");
    }
    std::memcpy(&out, bytes.data(), sizeof(Record));
}

constexpr std::size_t HEADER_ALIGN = 64;

std::size_t header_size(std::uint32_t members, bool table) {
    std::size_t size = members * sizeof(Record);
    if (table) size += (members + std::size_t{1}) * sizeof(std::uint64_t);
    return (size + HEADER_ALIGN - 1) / HEADER_ALIGN * HEADER_ALIGN;
}

/// The table message of a slot, built once at group rank 0: the records, the
/// offsets of the contributions when the call carries a table, and then the
/// contributions. The send to every member borrows it, and so does the table
/// that rank 0 reports.
struct Wire {
    dstd::Vector<std::byte> bytes;
    dstd::Vector<std::uint64_t> offsets;
    std::size_t data_at = 0;
};

Table table_of(const dstd::SharedPtr<const Wire>& wire) {
    Table table;
    table.data = dstd::Span<const std::byte>(
        wire->bytes.data() + wire->data_at, wire->bytes.size() - wire->data_at);
    table.offsets = dstd::Span<const std::uint64_t>(wire->offsets.data(),
                                                    wire->offsets.size());
    table.keep = wire;
    return table;
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

/// The bit of a byte of @ref Slot::seen that says the messages of the member
/// landed.
constexpr std::uint8_t LANDED = 2;

/// What the provider knows of one slot.
struct Slot {
    Slot(const SlotKey& slot_key, std::uint32_t member_count)
        : key(slot_key),
          members(member_count),
          records(member_count),
          seen(member_count, 0),
          staged(member_count) {}

    SlotKey key;
    std::uint32_t members = 0;
    /// Members that arrived, by their record or by their own arrival; each
    /// counts once.
    std::uint32_t arrived = 0;
    /// Rank 0 of the group arrived in this process, so this provider collects
    /// the slot and can see every member.
    bool collecting = false;
    bool ended = false;
    /// Set once the table of the slot is being built, so only one callback
    /// builds it.
    bool sealed = false;
    /// The member of the first arrival. Its call is the base of the comparison,
    /// so every later arrival is compared against the same record.
    std::uint32_t anchor = 0;
    dstd::Vector<Record> records;
    /// One byte per member: bit 0 for a member that arrived, bit 1 (@ref
    /// LANDED) once everything the member sends group rank 0 for this slot is
    /// in. A member of this process that called is seen at once, while its
    /// messages can still be on their way or lost.
    dstd::Vector<std::uint8_t> seen;
    /// The records group rank 0 took from messages: a member that arrived in
    /// this process still has to send its own before the table is built.
    std::uint32_t records_in = 0;
    /// Members whose record carries a table, and how many of their payloads are
    /// in.
    std::uint32_t payloads_expected = 0;
    std::uint32_t payloads_in = 0;
    bool has_table = false;
    bool combines = false;
    /// The contribution of group rank 0, which stays with its caller's keep.
    Contribution own;
    /// The payloads other members sent, copied out of their messages.
    dstd::Vector<dstd::Vector<std::byte>> staged;
    /// The fold in arrival order, for a call that combines, which the record
    /// of group rank 0 decides. Shared with the caller that folds, which runs
    /// outside the provider mutex.
    dstd::SharedPtr<ArrivalFold> fold;
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
                Contribution&& contribution, Done done) override {
        dstd::SharedPtr<Slot> slot;
        Arrival broken;
        bool collecting = false;
        bool folder = false;
        ArrivalFold::Batch batch;
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
            if (rank == 0) slot->seen[0] |= LANDED;
            if (rank == 0 && agrees && (record.flags & RECORD_TABLE) != 0)
                folder =
                    keep_own(*slot, record, std::move(contribution), batch);
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
            finish_slot(slot, broken, rank, nullptr, Table{});
            return false;
        }
        if (rank == 0) {
            if (folder) run_fold(slot, batch);
            if (collecting) collect(slot);
        } else {
            contribute(slot, rank, std::move(contribution));
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
            for (const dstd::SharedPtr<Slot>& slot : gone) {
                slot->ended = true;
                if (slot->fold) slot->fold->cancel_locked();
            }
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
            if (gone->fold) gone->fold->cancel_locked();
            slots_.erase(at);
            // Only a rank 0 of this process saw every member; any other rank
            // would guess, so it reports nothing. A member counts as arrived
            // when its messages are in, so a lost one names its sender.
            if (gone->seen[0] != 0) {
                mask = gone->seen;
                for (std::uint32_t member = 1; member < gone->members; ++member)
                    mask[member] = (gone->seen[member] & LANDED) != 0 ? 1 : 0;
            }
        }
        if (arrived != nullptr) *arrived = std::move(mask);
        // Outside the lock, since the parked messages answer it here.
        messaging_->abort(group_id, dstd::Error{dstd::Condition::Cancelled,
                                                "the collective ended"});
    }

   private:
    static std::uint64_t record_index(std::uint64_t seq) { return 2 * seq; }
    static std::uint64_t payload_index(std::uint64_t seq) {
        return 2 * seq + 1;
    }

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
        if ((record.flags & RECORD_TABLE) != 0) {
            slot.has_table = true;
            ++slot.payloads_expected;
        }
        return opens || slot.records[slot.anchor].kind == record.kind;
    }

    /// Keeps the contribution of group rank 0, or hands it to the fold when
    /// its record asks to combine. The slot then combines with the merge of
    /// rank 0, the one the collector can run: a function does not cross
    /// processes. Gives true when the caller folds. Called under the mutex.
    static bool keep_own(Slot& slot, const Record& record,
                         Contribution contribution, ArrivalFold::Batch& batch) {
        ++slot.payloads_in;
        if ((record.flags & RECORD_COMBINE) == 0 || !contribution.combine) {
            slot.own = std::move(contribution);
            return false;
        }
        slot.combines = true;
        slot.fold = dstd::make_shared<ArrivalFold>();
        slot.fold->set_combine_locked(std::move(contribution.combine));
        return slot.fold->take_locked(
            ArrivalFold::Held::borrowed(contribution.bytes,
                                        std::move(contribution.keep)),
            batch);
    }

    /// Folds @p batch and every batch that queued behind it, outside the
    /// mutex, then tries to complete the slot.
    void run_fold(const dstd::SharedPtr<Slot>& slot,
                  ArrivalFold::Batch& batch) {
        const dstd::SharedPtr<ArrivalFold> fold = slot->fold;
        for (bool again = true; again;) {
            fold->apply(batch);
            std::lock_guard<std::mutex> lock(mutex_);
            again = fold->refill_locked(batch);
        }
        broadcast(slot);
    }

    /// Whether every record and every payload the slot expects is in. Called
    /// under the mutex.
    static bool complete_locked(const Slot& slot) {
        return slot.records_in + 1 == slot.members &&
               slot.payloads_in == slot.payloads_expected &&
               !(slot.fold && slot.fold->busy_locked());
    }

    /// Sends the record and the payload of @p rank to group rank 0 and asks for
    /// the table back.
    void contribute(const dstd::SharedPtr<Slot>& slot, std::uint32_t rank,
                    Contribution contribution) {
        dstd::Vector<std::byte> bytes;
        bool table = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot->ended) return;
            bytes = pack_record(slot->records[rank]);
            table = (slot->records[rank].flags & RECORD_TABLE) != 0;
        }
        const MessageKey key{slot->key.group, rank, 0,
                             message_tag(TagPurpose::CollectiveRecord,
                                         record_index(slot->key.seq))};
        messaging_->send(key, std::move(bytes), HOLD_EVERYTHING,
                         failed_send(slot, rank));
        if (table) {
            const MessageKey payload_key{
                slot->key.group, rank, 0,
                message_tag(TagPurpose::CollectiveRecord,
                            payload_index(slot->key.seq))};
            messaging_->send(payload_key,
                             Payload::borrowed(contribution.bytes,
                                               std::move(contribution.keep)),
                             HOLD_EVERYTHING, failed_send(slot, rank));
        }
        take_table(slot, rank);
    }

    Messaging::SendDone failed_send(const dstd::SharedPtr<Slot>& slot,
                                    std::uint32_t rank) {
        return [this, slot, rank](const dstd::Error* why) {
            if (why != nullptr)
                finish_slot(slot, Arrival{}, rank, why, Table{});
        };
    }

    void take_table(const dstd::SharedPtr<Slot>& slot, std::uint32_t rank) {
        const MessageKey key{
            slot->key.group, 0, rank,
            message_tag(TagPurpose::CollectiveTable, slot->key.seq)};
        Messaging::RecvDone took = [this, slot, rank](
                                       dstd::Span<const std::byte> bytes,
                                       const dstd::Error* why) {
            if (why != nullptr) {
                finish_slot(slot, Arrival{}, rank, why, Table{});
                return;
            }
            Table table;
            dstd::Error malformed;
            bool parsed = false;
            bool personal = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Ended under this lock, so another callback already finished
                // the slot and the span its completion reads is gone.
                if (slot->ended) return;
                parsed = parse_table(*slot, rank, bytes, table, malformed);
                personal = (slot->records[rank].flags & RECORD_PERSONAL) != 0;
            }
            dstd::Vector<Table> tables;
            if (parsed && personal && table.size() != 0) {
                tables.resize(slot->members);
                tables[rank] = std::move(table);
                table = Table{};
            }
            finish_slot(slot, Arrival{}, rank, parsed ? nullptr : &malformed,
                        std::move(table), std::move(tables));
        };
        messaging_->recv(key, std::move(took));
    }

    /// Reads the table message into the records of @p slot and gives the
    /// table it holds in @p out. The message is a view for this call only, so a
    /// member that takes the table copies its contributions out: the one copy
    /// of them that this member makes. A message that does not describe a table
    /// of this slot gives false with the error. A complete table means every
    /// member arrived. Called under the mutex.
    static bool parse_table(Slot& slot, std::uint32_t rank,
                            dstd::Span<const std::byte> bytes, Table& out,
                            dstd::Error& why) {
        const bool table = (slot.records[rank].flags & RECORD_TABLE) != 0;
        const bool wants = (slot.records[rank].flags & RECORD_WANTS) != 0;
        const std::size_t head = header_size(slot.members, table);
        auto malformed = [&](const char* what) {
            why = dstd::Error::from_format<"a collective table {}">(
                dstd::Condition::InvalidArgument, dstd::StringView(what));
            return false;
        };
        if (bytes.size() < head) return malformed("is shorter than its header");
        dstd::Vector<std::uint64_t> offsets;
        if (table) {
            offsets.resize(slot.members + std::size_t{1});
            std::memcpy(offsets.data(),
                        bytes.data() + slot.members * sizeof(Record),
                        offsets.size() * sizeof(std::uint64_t));
            if (offsets[0] != 0) return malformed("does not start at 0");
            for (std::size_t at = 1; at < offsets.size(); ++at)
                if (offsets[at] < offsets[at - 1])
                    return malformed("has offsets that go back");
            const std::uint64_t room = bytes.size() - head;
            if (wants ? offsets.back() != room : room != 0)
                return malformed("does not hold the bytes its offsets name");
        } else if (bytes.size() != head) {
            return malformed("has bytes behind a header that names none");
        }
        for (std::uint32_t member = 0; member < slot.members; ++member) {
            unpack_record(
                bytes.subspan(member * sizeof(Record), sizeof(Record)),
                slot.records[member]);
        }
        slot.arrived = slot.members;
        if (!table || !wants) {
            out = Table{};
            return true;
        }
        auto buffer = dstd::make_shared<TableBuffer>();
        buffer->offsets = std::move(offsets);
        buffer->data.assign(bytes.begin() + head, bytes.end());
        out = make_table(std::move(buffer));
        return true;
    }

    /// Receives the record of every other member, as group rank 0.
    void collect(const dstd::SharedPtr<Slot>& slot) {
        for (std::uint32_t member = 1; member < slot->members; ++member) {
            const MessageKey key{slot->key.group, member, 0,
                                 message_tag(TagPurpose::CollectiveRecord,
                                             record_index(slot->key.seq))};
            Messaging::RecvDone took = [this, slot, member](
                                           dstd::Span<const std::byte> bytes,
                                           const dstd::Error* why) {
                if (why != nullptr) {
                    finish_slot(slot, Arrival{}, 0, why, Table{});
                    return;
                }
                bool table = false;
                Arrival broken;
                dstd::Error bad;
                bool is_bad = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (slot->ended) return;
                    Record record;
                    if (bytes.size() != sizeof(Record)) {
                        is_bad = true;
                        bad = dstd::Error::from_format<
                            "a collective record from rank {} has {} bytes">(
                            dstd::Condition::InvalidArgument, member,
                            bytes.size());
                    } else {
                        unpack_record(bytes, record);
                        ++slot->records_in;
                        if (arrive_locked(*slot, member, record)) {
                            table = (record.flags & RECORD_TABLE) != 0;
                            if (!table) slot->seen[member] |= LANDED;
                        } else {
                            broken = slot->seen;
                        }
                    }
                }
                if (is_bad) {
                    finish_slot(slot, Arrival{}, member, &bad, Table{});
                    return;
                }
                if (!broken.empty()) {
                    finish_slot(slot, broken, member, nullptr, Table{});
                    return;
                }
                if (table) take_payload(slot, member);
                broadcast(slot);
            };
            messaging_->recv(key, std::move(took));
        }
        broadcast(slot);
    }

    /// Receives the payload of @p member, as group rank 0.
    void take_payload(const dstd::SharedPtr<Slot>& slot, std::uint32_t member) {
        const MessageKey key{slot->key.group, member, 0,
                             message_tag(TagPurpose::CollectiveRecord,
                                         payload_index(slot->key.seq))};
        Messaging::RecvDone took = [this, slot, member](
                                       dstd::Span<const std::byte> bytes,
                                       const dstd::Error* why) {
            if (why != nullptr) {
                finish_slot(slot, Arrival{}, 0, why, Table{});
                return;
            }
            // Outside the lock: the view is valid for this call only, so the
            // copy of a large payload is made here and not under the mutex.
            // The fold of a slot that combines may run on another thread, so
            // it takes the copy too.
            dstd::Vector<std::byte> copy(bytes.begin(), bytes.end());
            bool folder = false;
            ArrivalFold::Batch batch;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (slot->ended) return;
                ++slot->payloads_in;
                slot->seen[member] |= LANDED;
                // Rank 0 decided whether the slot combines, so a member whose
                // record says otherwise is staged, and the records report the
                // mismatch.
                if (slot->combines)
                    folder = slot->fold->take_locked(
                        ArrivalFold::Held::owning(std::move(copy)), batch);
                else
                    slot->staged[member] = std::move(copy);
            }
            if (folder) run_fold(slot, batch);
            broadcast(slot);
        };
        messaging_->recv(key, std::move(took));
    }

    /// Sends the table of @p slot to every other member, as group rank 0, once
    /// every record and payload is in.
    void broadcast(const dstd::SharedPtr<Slot>& slot) {
        dstd::SharedPtr<Wire> wire;
        WireInput input;
        dstd::Vector<std::uint8_t> wants;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot->ended || slot->sealed || !complete_locked(*slot)) return;
            slot->sealed = true;
            input = take_wire_input(*slot);
            wants.resize(slot->members);
            for (std::uint32_t member = 0; member < slot->members; ++member)
                wants[member] =
                    (slot->records[member].flags & RECORD_WANTS) != 0;
        }
        if (input.personal) {
            broadcast_personal(slot, input, wants);
            return;
        }
        // Outside the mutex: the copy of every contribution takes as long as
        // it takes, and the slot is sealed, so nothing else reads them.
        wire = build_wire(input);
        // Ended before the sends: a member of this process whose receive they
        // answer would otherwise end the slot with the header it asked for.
        const dstd::SharedPtr<const Wire> shared = wire;
        finish_slot(slot, Arrival{}, 0, nullptr,
                    slot->has_table ? table_of(shared) : Table{});
        // Every member, even after a peer ended the slot here: the receive of a
        // peer that already ended stays parked otherwise. Each send borrows the
        // one wire; a member that does not take the table gets its header
        // only.
        const std::size_t head = header_size(slot->members, slot->has_table);
        for (std::uint32_t member = 1; member < slot->members; ++member) {
            const std::size_t size =
                wants[member] ? shared->bytes.size() : head;
            const MessageKey key{
                slot->key.group, 0, member,
                message_tag(TagPurpose::CollectiveTable, slot->key.seq)};
            messaging_->send(key,
                             Payload::borrowed(dstd::Span<const std::byte>(
                                                   shared->bytes.data(), size),
                                               shared),
                             HOLD_EVERYTHING, failed_send(slot, member));
        }
    }

    /// What the table message of a complete slot is built from, taken out of
    /// the slot under the mutex.
    struct WireInput {
        dstd::Vector<Record> records;
        Contribution own;
        dstd::Vector<dstd::Vector<std::byte>> staged;
        dstd::Vector<std::byte> folded;
        std::uint32_t members = 0;
        bool has_table = false;
        bool combines = false;
        bool personal = false;
    };

    /// Moves what the table message needs out of the sealed @p slot. Called
    /// under the mutex.
    static WireInput take_wire_input(Slot& slot) {
        WireInput input;
        input.records = slot.records;
        input.own = std::move(slot.own);
        input.staged = std::move(slot.staged);
        if (slot.combines) input.folded = slot.fold->take_folded();
        input.members = slot.members;
        input.has_table = slot.has_table;
        input.combines = slot.combines;
        for (const Record& record : slot.records)
            if ((record.flags & RECORD_PERSONAL) != 0) input.personal = true;
        return input;
    }

    /// Sends each member of a sealed personal slot the table message of its own
    /// pieces, and reports the one of rank 0. A member gets only the bytes
    /// addressed to it, so no message holds the whole exchange.
    void broadcast_personal(const dstd::SharedPtr<Slot>& slot, WireInput& input,
                            const dstd::Vector<std::uint8_t>& wants) {
        dstd::Vector<dstd::SharedPtr<Wire>> wires;
        dstd::Error bad;
        if (!build_personal_wires(input, wires, bad)) {
            finish_slot(slot, Arrival{}, 0, &bad, Table{});
            return;
        }
        // Rank 0 holds every member's table message to send it, so the ranks
        // that share this provider end with theirs.
        dstd::Vector<Table> tables(input.members);
        for (std::uint32_t member = 0; member < input.members; ++member)
            if (wants[member] != 0)
                tables[member] =
                    table_of(dstd::SharedPtr<const Wire>(wires[member]));
        finish_slot(slot, Arrival{}, 0, nullptr, Table{}, std::move(tables));
        const std::size_t head = header_size(input.members, true);
        for (std::uint32_t member = 1; member < input.members; ++member) {
            const dstd::SharedPtr<const Wire> wire = wires[member];
            const std::size_t size = wants[member] ? wire->bytes.size() : head;
            const MessageKey key{
                slot->key.group, 0, member,
                message_tag(TagPurpose::CollectiveTable, slot->key.seq)};
            messaging_->send(key,
                             Payload::borrowed(dstd::Span<const std::byte>(
                                                   wire->bytes.data(), size),
                                               wire),
                             HOLD_EVERYTHING, failed_send(slot, member));
        }
    }

    /// The table message of every member of a personal slot: the records, the
    /// offsets of the pieces addressed to that member, and the pieces, taken in
    /// source order. Each piece is copied once, from the copy out of its
    /// message. Gives false with the error when a contribution does not hold
    /// one piece per member.
    static bool build_personal_wires(WireInput& slot,
                                     dstd::Vector<dstd::SharedPtr<Wire>>& wires,
                                     dstd::Error& why) {
        const std::uint32_t members = slot.members;
        dstd::Vector<dstd::Span<const std::byte>> sources(members);
        for (std::uint32_t member = 0; member < members; ++member) {
            sources[member] = member == 0 ? slot.own.bytes
                                          : dstd::Span<const std::byte>(
                                                slot.staged[member].data(),
                                                slot.staged[member].size());
            const std::uint32_t flags = slot.records[member].flags;
            const bool ok = (flags & RECORD_PERSONAL) != 0 &&
                            (flags & RECORD_TABLE) != 0 && !slot.combines &&
                            personal_fits(sources[member], members);
            if (ok) continue;
            why = dstd::Error::from_format<
                "the contribution of rank {} does not hold one piece per "
                "member">(dstd::Condition::InvalidArgument, member);
            return false;
        }
        const std::size_t head = header_size(members, true);
        dstd::Vector<std::uint64_t> cursor(members);
        for (std::uint32_t member = 0; member < members; ++member)
            cursor[member] = std::uint64_t{members} * sizeof(std::uint64_t);
        wires.reserve(members);
        for (std::uint32_t dest = 0; dest < members; ++dest) {
            auto wire = dstd::make_shared<Wire>();
            wire->data_at = head;
            std::uint64_t total = 0;
            for (std::uint32_t member = 0; member < members; ++member)
                total += personal_size(sources[member], dest);
            wire->bytes.reserve(head + total);
            wire->bytes.resize(head);
            std::memcpy(wire->bytes.data(), slot.records.data(),
                        members * sizeof(Record));
            wire->offsets.push_back(0);
            for (std::uint32_t member = 0; member < members; ++member) {
                const std::uint64_t size = personal_size(sources[member], dest);
                const dstd::Span<const std::byte> piece =
                    sources[member].subspan(cursor[member], size);
                cursor[member] += size;
                wire->bytes.insert(wire->bytes.end(), piece.begin(),
                                   piece.end());
                wire->offsets.push_back(wire->offsets.back() + size);
            }
            std::memcpy(wire->bytes.data() + members * sizeof(Record),
                        wire->offsets.data(),
                        wire->offsets.size() * sizeof(std::uint64_t));
            wires.push_back(std::move(wire));
        }
        return true;
    }

    /// The table message of a complete slot: the one copy of every
    /// contribution at rank 0 beyond the copy out of each message, in a buffer
    /// the sends and the reported table share.
    static dstd::SharedPtr<Wire> build_wire(WireInput& slot) {
        auto wire = dstd::make_shared<Wire>();
        const std::size_t head = header_size(slot.members, slot.has_table);
        wire->data_at = head;
        std::size_t total = 0;
        const dstd::Vector<std::byte>& folded = slot.folded;
        auto bytes_of = [&slot, &folded](std::uint32_t member) {
            if (slot.combines) {
                return dstd::Span<const std::byte>(folded.data(),
                                                   folded.size());
            }
            if (member == 0) return slot.own.bytes;
            return dstd::Span<const std::byte>(slot.staged[member].data(),
                                               slot.staged[member].size());
        };
        if (slot.has_table) {
            if (slot.combines) {
                total = folded.size();
            } else {
                for (std::uint32_t member = 0; member < slot.members;
                     ++member) {
                    if ((slot.records[member].flags & RECORD_TABLE) != 0)
                        total += bytes_of(member).size();
                }
            }
        }
        wire->bytes.reserve(head + total);
        wire->bytes.resize(slot.members * sizeof(Record));
        std::memcpy(wire->bytes.data(), slot.records.data(),
                    slot.members * sizeof(Record));
        if (slot.has_table) {
            std::uint64_t at = 0;
            wire->offsets.push_back(0);
            if (slot.combines) {
                wire->offsets.push_back(folded.size());
                // A fold has one entry, so the other offsets repeat its end.
                for (std::uint32_t member = 1; member < slot.members; ++member)
                    wire->offsets.push_back(folded.size());
            } else {
                for (std::uint32_t member = 0; member < slot.members;
                     ++member) {
                    if ((slot.records[member].flags & RECORD_TABLE) != 0)
                        at += bytes_of(member).size();
                    wire->offsets.push_back(at);
                }
            }
            const std::size_t at_offsets = wire->bytes.size();
            wire->bytes.resize(at_offsets +
                               wire->offsets.size() * sizeof(std::uint64_t));
            std::memcpy(wire->bytes.data() + at_offsets, wire->offsets.data(),
                        wire->offsets.size() * sizeof(std::uint64_t));
        }
        wire->bytes.resize(head);
        if (slot.has_table) {
            if (slot.combines) {
                wire->bytes.insert(wire->bytes.end(), folded.begin(),
                                   folded.end());
            } else {
                for (std::uint32_t member = 0; member < slot.members;
                     ++member) {
                    if ((slot.records[member].flags & RECORD_TABLE) == 0)
                        continue;
                    const dstd::Span<const std::byte> part = bytes_of(member);
                    wire->bytes.insert(wire->bytes.end(), part.begin(),
                                       part.end());
                }
            }
        }
        return wire;
    }

    /// Ends @p slot once, with @p seen empty when every member arrived, and
    /// reports it outside the lock so the caller's group lock is free.
    void finish_slot(const dstd::SharedPtr<Slot>& slot, const Arrival& seen,
                     std::uint32_t rank, const dstd::Error* why, Table table,
                     dstd::Vector<Table> tables = {}) {
        Done done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot->ended) return;
            slot->ended = true;
            if (slot->fold) slot->fold->cancel_locked();
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
            dstd::Span<const std::uint8_t>(mask.data(), mask.size()),
            rank,
            why,
            std::move(table),
            dstd::Span<const Table>(tables.data(), tables.size())};
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
