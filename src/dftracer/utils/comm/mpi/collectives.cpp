// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/mpi/collectives.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <mpi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::mpi {

namespace {

using backend::Contribution;
using backend::Record;
using backend::SlotEnd;
using backend::Table;
using backend::TableBuffer;
using detail::CallKind;

/// The tags of the arrival questions on the messages communicator; the
/// messaging uses 1 and 2.
constexpr int TAG_QUERY = 3;
constexpr int TAG_REPLY = 4;

/// The most bytes one native call moves unless a test lowers it, and the number
/// of tags that tell the creations of communicators apart.
constexpr std::uint64_t DEFAULT_CHUNK_BYTES = std::uint64_t{1} << 30;
constexpr std::uint64_t TAG_SPAN = 16384;

/// The first tag of a creation: the tags below it belong to the messages and
/// the arrival questions on the same communicator.
constexpr std::uint64_t TAG_FIRST_CREATE = 16;

constexpr std::size_t KINDS = 16;

/// What each member gives the first collective of a slot: its record and the
/// size of its contribution.
struct Wire1 {
    Record record;
    std::uint64_t size = 0;
};

static_assert(sizeof(Wire1) == 48);

/// Asks a member whether it reached call @p seq of @p group.
struct Query {
    std::uint64_t nonce = 0;
    std::uint64_t group = 0;
    std::uint64_t seq = 0;
    std::uint32_t requester = 0;
    std::uint32_t reserved = 0;
};

struct Reply {
    std::uint64_t nonce = 0;
    std::uint32_t responder = 0;
    std::uint32_t reached = 0;
};

struct SlotKey {
    std::uint64_t group = 0;
    std::uint64_t seq = 0;

    friend bool operator<(const SlotKey& a, const SlotKey& b) noexcept {
        if (a.group != b.group) return a.group < b.group;
        return a.seq < b.seq;
    }
};

/// One call of this process at one slot.
struct Slot {
    Slot(const SlotKey& slot_key, std::uint32_t member_count,
         std::uint32_t member_rank, const Record& call_record,
         Contribution&& call_contribution, backend::Collectives::Done end)
        : key(slot_key),
          members(member_count),
          rank(member_rank),
          record(call_record),
          contribution(std::move(call_contribution)),
          done(std::move(end)) {}

    SlotKey key;
    std::uint32_t members = 0;
    std::uint32_t rank = 0;
    Record record;
    Contribution contribution;
    backend::Collectives::Done done;
    /// Set once by whoever ends the slot: its completion, or a drop that
    /// discards the result.
    std::atomic<bool> ended{false};
};

using SlotPtr = dstd::SharedPtr<Slot>;

struct Group;
using GroupPtr = dstd::SharedPtr<Group>;

/// The communicator of a group and its queue of slots. The fields are used on
/// the progress thread only, except those noted.
struct Group {
    enum State { Idle, Creating, Ready, Failed };
    std::uint64_t id = 0;
    dstd::Vector<std::uint32_t> members;
    MPI_Comm comm = MPI_COMM_NULL;
    bool own_comm = false;
    State state = Idle;
    dstd::Optional<dstd::Error> failed;
    std::uint64_t next_post = 0;
    bool busy = false;
    bool released = false;
    /// The native requests of the group that did not complete yet.
    std::size_t inflight = 0;
    /// The group a split made this one from, the slot of that split, and the
    /// ranks of the members in the parent.
    GroupPtr parent;
    std::uint64_t parent_seq = 0;
    dstd::Vector<int> parent_ranks;
    /// What keeps the next slot from starting: the end of a split slot, which
    /// binds the children, and each child that is still being created from
    /// this group's communicator. Changed on the completion thread too.
    std::atomic<int> hold{0};
    dstd::Map<std::uint64_t, SlotPtr> waiting;
};

/// The group and slot whose end the calling thread is delivering, so that the
/// children a split binds find their parent.
struct Delivering {
    GroupPtr group;
    std::uint64_t seq = 0;
};

thread_local const Delivering* t_delivering = nullptr;

/// What a deadline collects from the members.
struct Probe {
    std::mutex mutex;
    std::condition_variable wake;
    dstd::Vector<std::uint32_t> members;
    dstd::Vector<std::uint8_t> mask;
    std::size_t waiting = 0;
};

/// The memory a broadcast table views: the offsets and, at a member that
/// received the value, the buffer, or at the root the contribution itself.
struct BcastBlock {
    dstd::Vector<std::uint64_t> offsets;
    dstd::Vector<std::byte> data;
    dstd::SharedPtr<const void> held;
};

/// What each member tells each member before the pieces of a personal call
/// move: the size of the piece, the biggest piece of the sender, and whether
/// the sender's block holds one piece per member.
struct Piece {
    std::uint64_t size = 0;
    std::uint64_t biggest = 0;
    std::uint32_t bad = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(Piece) == 24);

/// The counts, displacements and staging memory of one round of a call.
struct Round {
    dstd::Vector<int> counts;
    dstd::Vector<int> displs;
    dstd::Vector<int> counts_in;
    dstd::Vector<int> displs_in;
    dstd::Vector<std::byte> out;
    dstd::Vector<std::byte> in;
};

/// What one slot holds while its collectives run.
struct Run {
    SlotPtr slot;
    GroupPtr group;
    Wire1 own;
    dstd::Vector<Wire1> all;
    dstd::Vector<Record> records;
    dstd::Vector<std::uint64_t> sizes;
    dstd::Vector<Piece> pieces_out;
    dstd::Vector<Piece> pieces_in;
    dstd::SharedPtr<TableBuffer> buffer;
    dstd::SharedPtr<BcastBlock> block;
    dstd::Optional<dstd::Error> error;
    /// The rounds that did not complete yet, and what ends the call when the
    /// last one does. Used on the progress thread only.
    std::size_t left = 0;
    /// Takes the run itself, so the closure holds no reference to it.
    dstd::Function<void(const dstd::SharedPtr<Run>&)> finish;
};

using RunPtr = dstd::SharedPtr<Run>;

dstd::Error mpi_failure(dstd::StringView call, int code) {
    return dstd::Error::from_format<"{} failed with MPI error {}">(
        dstd::Condition::Internal, call, code);
}

/// The MPI type of element code @p code, or MPI_DATATYPE_NULL.
MPI_Datatype mpi_type(std::uint32_t code) {
    switch (code) {
        case detail::ELEM_INT8:
            return MPI_INT8_T;
        case detail::ELEM_INT16:
            return MPI_INT16_T;
        case detail::ELEM_INT32:
            return MPI_INT32_T;
        case detail::ELEM_INT64:
            return MPI_INT64_T;
        case detail::ELEM_UINT8:
            return MPI_UINT8_T;
        case detail::ELEM_UINT16:
            return MPI_UINT16_T;
        case detail::ELEM_UINT32:
            return MPI_UINT32_T;
        case detail::ELEM_UINT64:
            return MPI_UINT64_T;
        case detail::ELEM_FLOAT32:
            return MPI_FLOAT;
        case detail::ELEM_FLOAT64:
            return MPI_DOUBLE;
        default:
            return MPI_DATATYPE_NULL;
    }
}

std::uint32_t type_size(std::uint32_t code) {
    switch (code) {
        case detail::ELEM_INT8:
        case detail::ELEM_UINT8:
            return 1;
        case detail::ELEM_INT16:
        case detail::ELEM_UINT16:
            return 2;
        case detail::ELEM_INT32:
        case detail::ELEM_UINT32:
        case detail::ELEM_FLOAT32:
            return 4;
        case detail::ELEM_INT64:
        case detail::ELEM_UINT64:
        case detail::ELEM_FLOAT64:
            return 8;
        default:
            return 0;
    }
}

bool is_float(std::uint32_t code) {
    return code == detail::ELEM_FLOAT32 || code == detail::ELEM_FLOAT64;
}

MPI_Op mpi_op(std::uint32_t reduce) {
    switch (reduce) {
        case detail::REDUCE_SUM:
            return MPI_SUM;
        case detail::REDUCE_MIN:
            return MPI_MIN;
        case detail::REDUCE_MAX:
            return MPI_MAX;
        default:
            return MPI_OP_NULL;
    }
}

/// Whether the records of every member name one call of one shape, which the
/// path that the first record chooses then fits every member.
bool same_shape(dstd::Span<const Record> records) {
    const Record& base = records[0];
    constexpr std::uint32_t FLAGS = backend::RECORD_TABLE |
                                    backend::RECORD_COMBINE |
                                    backend::RECORD_PERSONAL;
    for (std::size_t at = 1; at < records.size(); ++at) {
        const Record& other = records[at];
        if (other.kind != base.kind || other.root != base.root ||
            other.elem_size != base.elem_size || other.color != base.color ||
            other.reduce != base.reduce ||
            (other.flags & FLAGS) != (base.flags & FLAGS))
            return false;
    }
    return true;
}

class Impl : public backend::Collectives,
             public std::enable_shared_from_this<Impl> {
   public:
    explicit Impl(dstd::SharedPtr<Context> context)
        : context_(std::move(context)) {}

    /// Starts answering the questions of other members. Not in the
    /// constructor, which cannot name the shared owner.
    void attach() {
        poller_ = context_->add_poller(
            [self = shared_from_this()] { return self->poll(); });
    }

    void detach() { context_->remove_poller(poller_); }

    std::uint64_t native_slots(std::uint32_t kind) const noexcept override {
        return kind < KINDS ? by_kind_[kind].load(std::memory_order_relaxed)
                            : 0;
    }

    void limit_chunk_bytes(std::uint64_t bytes) noexcept override {
        limit_.store(bytes, std::memory_order_relaxed);
    }

    std::uint64_t native_folds() const noexcept override {
        return folds_.load(std::memory_order_relaxed);
    }

    /// Notes the members of the group, as world ranks in group order. A group
    /// of one member has its communicator at once. The child of a split is
    /// made from the communicator of its parent, at the end of the split slot
    /// and before the parent runs its next slot, so no request of the parent
    /// is in flight and every member makes it at the same point. Any other
    /// group makes its communicator at its first call. A group made by a split
    /// is bound by the members of its parent only.
    void bind_group(std::uint64_t group_id,
                    dstd::Span<const std::uint32_t> members) override {
        std::lock_guard<std::mutex> lock(mutex_);
        members_[group_id].assign(members.begin(), members.end());
        const std::uint32_t me = context_->rank();
        if (std::find(members.begin(), members.end(), me) == members.end())
            return;
        auto group = dstd::make_shared<Group>();
        group->id = group_id;
        group->members.assign(members.begin(), members.end());
        if (members.size() == 1) {
            group->comm = MPI_COMM_SELF;
            group->state = Group::Ready;
        } else if (t_delivering != nullptr) {
            const GroupPtr& parent = t_delivering->group;
            for (const std::uint32_t member : members) {
                const auto at = std::find(parent->members.begin(),
                                          parent->members.end(), member);
                group->parent_ranks.push_back(
                    static_cast<int>(at - parent->members.begin()));
            }
            group->parent = parent;
            group->parent_seq = t_delivering->seq;
            parent->hold.fetch_add(1, std::memory_order_acq_rel);
            begin_create(group);
        }
        groups_[group_id] = std::move(group);
    }

    bool arrive(std::uint64_t group_id, std::uint64_t seq,
                std::uint32_t members, std::uint32_t rank, const Record& record,
                Contribution&& contribution, Done done) override {
        auto slot = dstd::make_shared<Slot>(
            SlotKey{group_id, seq}, members, rank, record,
            std::move(contribution), std::move(done));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slots_[slot->key] = slot;
            std::uint64_t& reached = reached_[group_id];
            reached = std::max(reached, seq + 1);
        }
        context_->submit(
            [self = shared_from_this(), slot]() mutable { self->start(slot); });
        return true;
    }

    void drop(std::uint64_t group_id, std::uint64_t seq,
              backend::Arrival* arrived) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto at = slots_.find(SlotKey{group_id, seq});
            if (at != slots_.end()) {
                at->second->ended.store(true, std::memory_order_release);
                slots_.erase(at);
            }
        }
        if (arrived != nullptr) *arrived = ask(group_id, seq);
    }

    void release(std::uint64_t group_id) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto at = slots_.begin(); at != slots_.end();) {
                if (at->first.group != group_id) {
                    ++at;
                    continue;
                }
                at->second->ended.store(true, std::memory_order_release);
                at = slots_.erase(at);
            }
            reached_.erase(group_id);
            members_.erase(group_id);
        }
        context_->submit(
            [self = shared_from_this(), group_id] { self->forget(group_id); });
    }

   private:
    /// The bytes one native call of a group of @p members moves at most: the
    /// limit, and a share of what an int count holds so that the counts and
    /// displacements of a call over all members fit it.
    std::uint64_t chunk_for(std::uint32_t members) const {
        std::uint64_t limit = limit_.load(std::memory_order_relaxed);
        if (limit == 0) limit = DEFAULT_CHUNK_BYTES;
        const std::uint64_t share = (std::uint64_t{INT_MAX} / 2) / members;
        return std::max<std::uint64_t>(1, std::min(limit, share));
    }

    /// The bytes of round @p round of a block of @p size bytes cut in rounds of
    /// @p chunk.
    static std::uint64_t slice(std::uint64_t size, std::uint64_t round,
                               std::uint64_t chunk) {
        const std::uint64_t begin = round * chunk;
        return size > begin ? std::min(chunk, size - begin) : 0;
    }

    static std::uint64_t rounds_of(std::uint64_t biggest, std::uint64_t chunk) {
        return (biggest + chunk - 1) / chunk;
    }

    /// Makes the communicator of the group on a thread of its own. The members
    /// call MPI_Comm_create_group at different times and the call waits for
    /// all of them, so it holds neither the MPI lock nor a lock of the group.
    /// It counts as a pending native request until it is handled. The
    /// communicator is freed when the group is released and has no request.
    void begin_create(const GroupPtr& group) {
        group->state = Group::Creating;
        context_->native_begin();
        dstd::Thread worker(
            [self = shared_from_this(), group] {
                MPI_Comm comm = MPI_COMM_NULL;
                const int rc = self->make_comm(*group, comm);
                self->context_->submit([self, group, comm, rc] {
                    self->created(group, comm, rc);
                });
            },
            "comm_mpi_create");
        worker.detach();
    }

    /// Makes the communicator of @p group over its members in group order, so
    /// that the rank in it is the group rank. A child of a split comes from
    /// the communicator of its parent and takes the slot of the split as its
    /// tag: the creations on one parent never overlap in time, and siblings
    /// have no member in common. The world and node groups come from the
    /// messages communicator, and tell each other apart by their ids.
    int make_comm(const Group& group, MPI_Comm& out) {
        const bool child = group.parent != nullptr;
        const MPI_Comm from = child ? group.parent->comm : context_->messages();
        dstd::Vector<int> ranks = group.parent_ranks;
        if (!child) ranks.assign(group.members.begin(), group.members.end());
        const std::uint64_t tag = child
                                      ? group.parent_seq % TAG_SPAN
                                      : TAG_FIRST_CREATE + group.id % TAG_SPAN;
        MPI_Group base = MPI_GROUP_NULL;
        MPI_Group sub = MPI_GROUP_NULL;
        int rc = MPI_Comm_group(from, &base);
        if (rc == MPI_SUCCESS)
            rc = MPI_Group_incl(base, static_cast<int>(ranks.size()),
                                ranks.data(), &sub);
        if (rc == MPI_SUCCESS)
            rc = MPI_Comm_create_group(from, sub, static_cast<int>(tag), &out);
        if (sub != MPI_GROUP_NULL) MPI_Group_free(&sub);
        if (base != MPI_GROUP_NULL) MPI_Group_free(&base);
        return rc;
    }

    /// The communicator is made, or the creation failed: start the slots that
    /// waited for it, and let the parent go on.
    void created(const GroupPtr& group, MPI_Comm comm, int rc) {
        context_->native_end();
        if (rc != MPI_SUCCESS) {
            group->state = Group::Failed;
            group->failed = mpi_failure("MPI_Comm_create_group", rc);
            fail_waiting(group);
        } else {
            group->comm = comm;
            group->own_comm = true;
            group->state = Group::Ready;
            pump(group);
            maybe_free(group);
        }
        if (group->parent) {
            group->parent->hold.fetch_sub(1, std::memory_order_acq_rel);
            pump(group->parent);
            maybe_free(group->parent);
        }
    }

    /// Frees the communicator of a released group that has no request in
    /// flight and no child to make from it.
    void maybe_free(const GroupPtr& group) {
        if (!group->released || !group->own_comm ||
            group->state != Group::Ready || group->inflight > 0 ||
            group->hold.load(std::memory_order_acquire) > 0)
            return;
        MPI_Comm_free(&group->comm);
        group->own_comm = false;
    }

    void fail_waiting(const GroupPtr& group) {
        dstd::Map<std::uint64_t, SlotPtr> waiting = std::move(group->waiting);
        group->waiting.clear();
        for (auto& entry : waiting) {
            auto run = dstd::make_shared<Run>();
            run->slot = entry.second;
            fail(run, *group->failed, entry.second->rank);
        }
    }

    void forget(std::uint64_t group_id) {
        GroupPtr group = find_group(group_id);
        if (!group) return;
        group->released = true;
        group->waiting.clear();
        maybe_free(group);
        std::lock_guard<std::mutex> lock(mutex_);
        groups_.erase(group_id);
    }

    GroupPtr find_group(std::uint64_t group_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto at = groups_.find(group_id);
        return at == groups_.end() ? GroupPtr{} : at->second;
    }

    /// Takes the call of this process to its group queue.
    void start(const SlotPtr& slot) {
        const GroupPtr group = find_group(slot->key.group);
        if (!group || group->released) {
            auto run = dstd::make_shared<Run>();
            run->slot = slot;
            fail(run,
                 dstd::Error::from_format<
                     "group {} has no communicator on this rank">(
                     dstd::Condition::Internal, slot->key.group),
                 slot->rank);
            return;
        }
        group->waiting[slot->key.seq] = slot;
        if (group->state == Group::Idle) begin_create(group);
        if (group->state == Group::Failed) fail_waiting(group);
        pump(group);
    }

    /// Starts the next slot of the group when none is between its first and
    /// last post, so every member posts the slots in one order.
    void pump(const GroupPtr& group) {
        while (!group->busy && group->state == Group::Ready &&
               group->hold.load(std::memory_order_acquire) == 0) {
            auto at = group->waiting.find(group->next_post);
            if (at == group->waiting.end()) return;
            SlotPtr slot = std::move(at->second);
            group->waiting.erase(at);
            ++group->next_post;
            if (slot->ended.load(std::memory_order_acquire)) continue;
            group->busy = true;
            begin(slot, group);
        }
    }

    void release_turn(const GroupPtr& group) {
        group->busy = false;
        pump(group);
    }

    void begin(const SlotPtr& slot, const GroupPtr& group) {
        auto run = dstd::make_shared<Run>();
        run->slot = slot;
        run->group = group;
        run->own.record = slot->record;
        run->own.size = slot->contribution.bytes.size();
        run->all.resize(slot->members);
        MPI_Request request = MPI_REQUEST_NULL;
        const int rc =
            MPI_Iallgather(&run->own, sizeof(Wire1), MPI_BYTE, run->all.data(),
                           sizeof(Wire1), MPI_BYTE, group->comm, &request);
        if (rc != MPI_SUCCESS) {
            fail(run, mpi_failure("MPI_Iallgather", rc), slot->rank);
            release_turn(group);
            return;
        }
        track(group, request,
              [self = shared_from_this(), run] { self->gathered(run); });
    }

    /// Watches @p request of @p group, and runs @p next on the progress thread
    /// when it completed.
    void track(const GroupPtr& group, MPI_Request request, Context::Job next) {
        context_->native_begin();
        ++group->inflight;
        context_->watch(request, [self = shared_from_this(), group,
                                  after = std::move(next)]() mutable {
            self->context_->native_end();
            self->context_->submit(
                [self, group, after = std::move(after)]() mutable {
                    after();
                    --group->inflight;
                    self->maybe_free(group);
                });
        });
    }

    /// Watches one of the rounds of a call: @p copy runs when it completed, and
    /// the last round to complete runs the finish of the call. @p keep is what
    /// the request reads, which stays alive until the request completes.
    void track_round(const RunPtr& run, MPI_Request request, Context::Job copy,
                     dstd::SharedPtr<Round> keep = {}) {
        track(run->group, request,
              [self = shared_from_this(), run, keep = std::move(keep),
               copy = std::move(copy)]() mutable {
                  if (copy) copy();
                  if (--run->left == 0) run->finish(run);
              });
    }

    /// The records of every member are here: choose the path and post the
    /// next collectives, or end the slot.
    void gathered(const RunPtr& run) {
        const GroupPtr& group = run->group;
        const SlotPtr& slot = run->slot;
        const std::uint32_t n = slot->members;
        run->records.resize(n);
        run->sizes.resize(n);
        for (std::uint32_t at = 0; at < n; ++at) {
            run->records[at] = run->all[at].record;
            run->sizes[at] = run->all[at].size;
        }
        if (slot->ended.load(std::memory_order_acquire)) {
            release_turn(group);
            return;
        }
        const dstd::Span<const Record> records(run->records.data(), n);
        const Record& base = records[0];
        bool same_kind = true;
        bool all_table = true;
        for (const Record& record : records) {
            same_kind = same_kind && record.kind == base.kind;
            all_table = all_table && (record.flags & backend::RECORD_TABLE);
        }
        if (!same_kind || !all_table ||
            (base.flags & backend::RECORD_TABLE) == 0) {
            deliver(run, Table{}, {}, false, false);
            release_turn(group);
            return;
        }
        const bool uniform = same_shape(records);
        const CallKind kind = static_cast<CallKind>(base.kind);
        if (uniform && (base.flags & backend::RECORD_PERSONAL) != 0) {
            post_personal(run);
            return;
        }
        if (uniform &&
            (kind == CallKind::Reduce || kind == CallKind::AllReduce) &&
            reduces_natively(run)) {
            post_reduce(run);
            release_turn(group);
            return;
        }
        if (uniform && base.root < n &&
            (kind == CallKind::Broadcast || kind == CallKind::Scatter)) {
            post_bcast(run);
            release_turn(group);
            return;
        }
        post_gather(run, uniform && base.root < n && kind == CallKind::Gather);
        release_turn(group);
    }

    /// Whether the reduction has a native form that gives the fold in rank
    /// order bit for bit, or that the call allows in any order.
    bool reduces_natively(const RunPtr& run) const {
        const Record& base = run->records[0];
        if (mpi_op(base.reduce) == MPI_OP_NULL) return false;
        const std::uint32_t width = type_size(base.color);
        if (width == 0 || base.elem_size != width) return false;
        const bool any_order = (base.flags & backend::RECORD_COMBINE) != 0;
        if (is_float(base.color) && !any_order) return false;
        for (const std::uint64_t size : run->sizes)
            if (size != run->sizes[0]) return false;
        return run->sizes[0] % width == 0;
    }

    void post_reduce(const RunPtr& run) {
        const GroupPtr& group = run->group;
        const SlotPtr& slot = run->slot;
        const Record& base = run->records[0];
        const std::uint32_t n = slot->members;
        const bool all =
            static_cast<CallKind>(base.kind) == CallKind::AllReduce;
        const bool takes = all || slot->rank == base.root;
        const std::uint64_t width = type_size(base.color);
        const std::uint64_t elems = run->sizes[0] / width;
        const std::uint64_t step =
            std::max<std::uint64_t>(1, chunk_for(n) / width);
        const std::uint64_t rounds = rounds_of(elems, step);
        auto buffer = dstd::make_shared<TableBuffer>();
        buffer->offsets.push_back(0);
        buffer->offsets.push_back(run->sizes[0]);
        if (takes) buffer->data.resize(run->sizes[0]);
        run->buffer = buffer;
        auto finish = [self = shared_from_this(),
                       takes](const RunPtr& finished) {
            self->deliver(
                finished,
                takes ? backend::make_table(finished->buffer) : Table{}, {},
                true, false);
        };
        if (rounds == 0) {
            finish(run);
            return;
        }
        run->left = rounds;
        run->finish = finish;
        const char* call = all ? "MPI_Iallreduce" : "MPI_Ireduce";
        for (std::uint64_t round = 0; round < rounds; ++round) {
            const std::uint64_t first = round * step;
            const int count = static_cast<int>(std::min(step, elems - first));
            const void* send = slot->contribution.bytes.data() + first * width;
            void* recv = takes ? buffer->data.data() + first * width : nullptr;
            MPI_Request request = MPI_REQUEST_NULL;
            const int rc =
                all ? MPI_Iallreduce(send, recv, count, mpi_type(base.color),
                                     mpi_op(base.reduce), group->comm, &request)
                    : MPI_Ireduce(send, recv, count, mpi_type(base.color),
                                  mpi_op(base.reduce),
                                  static_cast<int>(base.root), group->comm,
                                  &request);
            if (rc != MPI_SUCCESS) {
                fail(run, mpi_failure(call, rc), slot->rank);
                return;
            }
            track_round(run, request, {});
        }
    }

    void post_bcast(const RunPtr& run) {
        const GroupPtr& group = run->group;
        const SlotPtr& slot = run->slot;
        const std::uint32_t n = slot->members;
        const std::uint32_t root = run->records[0].root;
        const std::uint64_t size = run->sizes[root];
        const std::uint64_t chunk = chunk_for(n);
        auto block = dstd::make_shared<BcastBlock>();
        block->offsets.resize(n + std::size_t{1}, 0);
        for (std::uint32_t at = root + 1; at <= n; ++at)
            block->offsets[at] = size;
        std::byte* data = nullptr;
        dstd::Span<const std::byte> view;
        if (slot->rank == root) {
            view = slot->contribution.bytes;
            data = const_cast<std::byte*>(view.data());
            block->held = slot->contribution.keep;
        } else {
            block->data.resize(size);
            view = dstd::Span<const std::byte>(block->data.data(), size);
            data = block->data.data();
        }
        Table table;
        table.data = view;
        table.offsets = dstd::Span<const std::uint64_t>(block->offsets.data(),
                                                        block->offsets.size());
        table.keep = block;
        run->block = block;
        auto finish = [self = shared_from_this(),
                       held = std::move(table)](const RunPtr& finished) {
            self->deliver(finished, held, {}, false, false);
        };
        const std::uint64_t rounds = rounds_of(size, chunk);
        if (rounds == 0) {
            finish(run);
            return;
        }
        run->left = rounds;
        run->finish = finish;
        for (std::uint64_t round = 0; round < rounds; ++round) {
            MPI_Request request = MPI_REQUEST_NULL;
            const int rc = MPI_Ibcast(
                data + round * chunk,
                static_cast<int>(slice(size, round, chunk)), MPI_BYTE,
                static_cast<int>(root), group->comm, &request);
            if (rc != MPI_SUCCESS) {
                fail(run, mpi_failure("MPI_Ibcast", rc), slot->rank);
                return;
            }
            track_round(run, request, {});
        }
    }

    /// Gathers every contribution into one buffer, at every member or at the
    /// root of a gather, in rounds of at most one chunk of each contribution.
    /// A call of one round receives into the buffer itself; with more rounds a
    /// count could not address it, so each round lands in a staging block and
    /// is copied in.
    void post_gather(const RunPtr& run, bool to_root) {
        const GroupPtr& group = run->group;
        const SlotPtr& slot = run->slot;
        const std::uint32_t n = slot->members;
        const std::uint32_t root = to_root ? run->records[0].root : 0;
        const bool takes = !to_root || slot->rank == root;
        const std::uint64_t chunk = chunk_for(n);
        std::uint64_t total = 0;
        std::uint64_t biggest = 0;
        for (const std::uint64_t size : run->sizes) {
            total += size;
            biggest = std::max(biggest, size);
        }
        auto buffer = dstd::make_shared<TableBuffer>();
        buffer->offsets.resize(n + std::size_t{1}, 0);
        for (std::uint32_t at = 0; at < n; ++at)
            buffer->offsets[at + 1] = buffer->offsets[at] + run->sizes[at];
        if (takes) buffer->data.resize(total);
        run->buffer = buffer;
        const bool fold =
            (run->records[0].flags & backend::RECORD_COMBINE) != 0;
        auto finish = [self = shared_from_this(), takes,
                       fold](const RunPtr& finished) {
            self->deliver(
                finished,
                takes ? backend::make_table(finished->buffer) : Table{}, {},
                false, fold);
        };
        const std::uint64_t rounds = rounds_of(biggest, chunk);
        if (rounds == 0) {
            finish(run);
            return;
        }
        run->left = rounds;
        run->finish = finish;
        const bool staged = rounds > 1;
        for (std::uint64_t round = 0; round < rounds; ++round) {
            auto step = dstd::make_shared<Round>();
            step->counts.resize(n);
            step->displs.resize(n);
            std::uint64_t at = 0;
            for (std::uint32_t member = 0; member < n; ++member) {
                step->counts[member] =
                    static_cast<int>(slice(run->sizes[member], round, chunk));
                step->displs[member] =
                    static_cast<int>(staged ? at : buffer->offsets[member]);
                at += step->counts[member];
            }
            if (staged && takes) step->in.resize(at);
            const void* send = slot->contribution.bytes.data() + round * chunk;
            void* recv =
                !takes ? nullptr
                       : (staged ? static_cast<void*>(step->in.data())
                                 : static_cast<void*>(buffer->data.data()));
            MPI_Request request = MPI_REQUEST_NULL;
            const int send_count = step->counts[slot->rank];
            const int rc =
                to_root
                    ? MPI_Igatherv(send, send_count, MPI_BYTE, recv,
                                   step->counts.data(), step->displs.data(),
                                   MPI_BYTE, static_cast<int>(root),
                                   group->comm, &request)
                    : MPI_Iallgatherv(send, send_count, MPI_BYTE, recv,
                                      step->counts.data(), step->displs.data(),
                                      MPI_BYTE, group->comm, &request);
            if (rc != MPI_SUCCESS) {
                fail(run,
                     mpi_failure(to_root ? "MPI_Igatherv" : "MPI_Iallgatherv",
                                 rc),
                     slot->rank);
                return;
            }
            Context::Job copy;
            if (staged && takes)
                copy = [run, step, round, chunk, n] {
                    for (std::uint32_t member = 0; member < n; ++member) {
                        if (step->counts[member] == 0) continue;
                        std::memcpy(
                            run->buffer->data.data() +
                                run->buffer->offsets[member] + round * chunk,
                            step->in.data() + step->displs[member],
                            static_cast<std::size_t>(step->counts[member]));
                    }
                };
            track_round(run, request, std::move(copy), step);
        }
    }

    /// The pieces each member addresses to each member. The first exchange
    /// gives every member the size of each piece it receives, the biggest piece
    /// of each sender and whether the sender's block is well formed, so all
    /// members decide the same: fail with the same error, or move the pieces in
    /// the same number of rounds.
    void post_personal(const RunPtr& run) {
        const GroupPtr& group = run->group;
        const SlotPtr& slot = run->slot;
        const std::uint32_t n = slot->members;
        const dstd::Span<const std::byte> bytes = slot->contribution.bytes;
        run->pieces_out.assign(n, Piece{});
        run->pieces_in.assign(n, Piece{});
        const bool fits = backend::personal_fits(bytes, n);
        std::uint64_t biggest = 0;
        if (fits)
            for (std::uint32_t dest = 0; dest < n; ++dest)
                biggest =
                    std::max(biggest, backend::personal_size(bytes, dest));
        for (std::uint32_t dest = 0; dest < n; ++dest) {
            run->pieces_out[dest].size =
                fits ? backend::personal_size(bytes, dest) : 0;
            run->pieces_out[dest].biggest = biggest;
            run->pieces_out[dest].bad = fits ? 0 : 1;
        }
        MPI_Request request = MPI_REQUEST_NULL;
        const int rc =
            MPI_Ialltoall(run->pieces_out.data(), sizeof(Piece), MPI_BYTE,
                          run->pieces_in.data(), sizeof(Piece), MPI_BYTE,
                          group->comm, &request);
        if (rc != MPI_SUCCESS) {
            fail(run, mpi_failure("MPI_Ialltoall", rc), slot->rank);
            release_turn(group);
            return;
        }
        track(group, request,
              [self = shared_from_this(), run] { self->sized(run); });
    }

    void sized(const RunPtr& run) {
        const GroupPtr& group = run->group;
        const SlotPtr& slot = run->slot;
        const std::uint32_t n = slot->members;
        if (slot->ended.load(std::memory_order_acquire)) {
            release_turn(group);
            return;
        }
        std::uint64_t total = 0;
        std::uint64_t biggest = 0;
        for (std::uint32_t from = 0; from < n; ++from) {
            const Piece& piece = run->pieces_in[from];
            if (piece.bad != 0) {
                fail(run,
                     dstd::Error::from_format<
                         "the contribution of rank {} does not hold one piece "
                         "per member">(dstd::Condition::InvalidArgument, from),
                     from);
                release_turn(group);
                return;
            }
            total += piece.size;
            biggest = std::max(biggest, piece.biggest);
        }
        const std::uint64_t chunk = chunk_for(n);
        const std::uint64_t rounds = rounds_of(biggest, chunk);
        auto buffer = dstd::make_shared<TableBuffer>();
        buffer->offsets.resize(n + std::size_t{1}, 0);
        for (std::uint32_t from = 0; from < n; ++from)
            buffer->offsets[from + 1] =
                buffer->offsets[from] + run->pieces_in[from].size;
        buffer->data.resize(total);
        run->buffer = buffer;
        auto finish = [self = shared_from_this()](const RunPtr& finished) {
            dstd::Vector<Table> tables(finished->slot->members);
            tables[finished->slot->rank] =
                backend::make_table(finished->buffer);
            self->deliver(finished, Table{}, std::move(tables), false, false);
        };
        if (rounds == 0) {
            finish(run);
            release_turn(group);
            return;
        }
        run->left = rounds;
        run->finish = finish;
        const bool staged = rounds > 1;
        const std::uint64_t head = std::uint64_t{n} * sizeof(std::uint64_t);
        dstd::Vector<std::uint64_t> start(n);
        std::uint64_t at = head;
        for (std::uint32_t dest = 0; dest < n; ++dest) {
            start[dest] = at;
            at += run->pieces_out[dest].size;
        }
        const dstd::Span<const std::byte> bytes = slot->contribution.bytes;
        for (std::uint64_t round = 0; round < rounds; ++round) {
            auto step = dstd::make_shared<Round>();
            step->counts.resize(n);
            step->displs.resize(n);
            step->counts_in.resize(n);
            step->displs_in.resize(n);
            std::uint64_t out_at = 0;
            std::uint64_t in_at = 0;
            for (std::uint32_t peer = 0; peer < n; ++peer) {
                const std::uint64_t out_size =
                    slice(run->pieces_out[peer].size, round, chunk);
                const std::uint64_t in_size =
                    slice(run->pieces_in[peer].size, round, chunk);
                step->counts[peer] = static_cast<int>(out_size);
                step->counts_in[peer] = static_cast<int>(in_size);
                step->displs[peer] = static_cast<int>(
                    staged ? out_at : start[peer] + round * chunk);
                step->displs_in[peer] =
                    static_cast<int>(staged ? in_at : buffer->offsets[peer]);
                out_at += out_size;
                in_at += in_size;
            }
            const void* send = bytes.data();
            if (staged) {
                step->out.resize(out_at);
                step->in.resize(in_at);
                for (std::uint32_t peer = 0; peer < n; ++peer) {
                    if (step->counts[peer] == 0) continue;
                    std::memcpy(step->out.data() + step->displs[peer],
                                bytes.data() + start[peer] + round * chunk,
                                static_cast<std::size_t>(step->counts[peer]));
                }
                send = step->out.data();
            }
            MPI_Request request = MPI_REQUEST_NULL;
            const int rc = MPI_Ialltoallv(
                send, step->counts.data(), step->displs.data(), MPI_BYTE,
                staged ? static_cast<void*>(step->in.data())
                       : static_cast<void*>(buffer->data.data()),
                step->counts_in.data(), step->displs_in.data(), MPI_BYTE,
                group->comm, &request);
            if (rc != MPI_SUCCESS) {
                fail(run, mpi_failure("MPI_Ialltoallv", rc), slot->rank);
                release_turn(group);
                return;
            }
            Context::Job copy;
            if (staged)
                copy = [run, step, round, chunk, n] {
                    for (std::uint32_t from = 0; from < n; ++from) {
                        if (step->counts_in[from] == 0) continue;
                        std::memcpy(
                            run->buffer->data.data() +
                                run->buffer->offsets[from] + round * chunk,
                            step->in.data() + step->displs_in[from],
                            static_cast<std::size_t>(step->counts_in[from]));
                    }
                };
            track_round(run, request, std::move(copy), step);
        }
        release_turn(group);
    }

    /// Ends the slot with its table, on the completion thread. @p combine asks
    /// to fold the entries in rank order with the merge of the call first.
    void deliver(const RunPtr& run, Table table, dstd::Vector<Table> tables,
                 bool folded, bool combine) {
        const std::uint32_t rank = run->slot->rank;
        if (!take(run)) return;
        const std::uint32_t kind =
            run->records.empty() ? 0 : run->records[0].kind;
        if (kind < KINDS)
            by_kind_[kind].fetch_add(1, std::memory_order_relaxed);
        if (folded) folds_.fetch_add(1, std::memory_order_relaxed);
        const bool splits = kind == static_cast<std::uint32_t>(CallKind::Split);
        if (splits) run->group->hold.fetch_add(1, std::memory_order_acq_rel);
        context_->complete([self = shared_from_this(), splits, run,
                            result = std::move(table),
                            pieces = std::move(tables), folded, combine,
                            rank]() mutable {
            if (combine && result.size() > 0) {
                auto merged = dstd::make_shared<TableBuffer>();
                const dstd::Span<const std::byte> first = result.at(0);
                merged->data.assign(first.begin(), first.end());
                backend::Combine& fold = run->slot->contribution.combine;
                if (fold) {
                    for (std::uint32_t at = 1; at < result.size(); ++at) {
                        const dstd::Span<const std::byte> in = result.at(at);
                        if (in.size() == merged->data.size())
                            fold(merged->data.data(), in.data(), in.size());
                    }
                }
                merged->offsets.push_back(0);
                merged->offsets.push_back(merged->data.size());
                result = backend::make_table(std::move(merged));
            }
            SlotEnd end;
            end.records = dstd::Span<const Record>(run->records.data(),
                                                   run->records.size());
            end.rank = rank;
            end.table = std::move(result);
            end.tables = dstd::Span<const Table>(pieces.data(), pieces.size());
            end.folded = folded;
            const Delivering delivering{run->group, run->slot->key.seq};
            const Delivering* const outer = t_delivering;
            t_delivering = &delivering;
            run->slot->done(end);
            t_delivering = outer;
            if (splits)
                self->context_->submit([self, run] {
                    run->group->hold.fetch_sub(1, std::memory_order_acq_rel);
                    self->pump(run->group);
                    self->maybe_free(run->group);
                });
        });
    }

    /// Ends the slot with @p why on the completion thread.
    void fail(const RunPtr& run, dstd::Error why, std::uint32_t rank) {
        if (!take(run)) return;
        run->error = std::move(why);
        context_->complete([run, rank]() mutable {
            SlotEnd end;
            end.rank = rank;
            end.why = &*run->error;
            run->slot->done(end);
        });
    }

    /// Claims the end of the slot, and gives false when a drop already ended
    /// it, which discards the result.
    bool take(const RunPtr& run) {
        if (run->slot->ended.exchange(true, std::memory_order_acq_rel))
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto at = slots_.find(run->slot->key);
        if (at != slots_.end() && at->second == run->slot) slots_.erase(at);
        return true;
    }

    /// Asks every other member of @p group whether it reached call @p seq, and
    /// gives one byte per member, 1 for a member that said yes. A member that
    /// did not answer in time counts as not arrived.
    backend::Arrival ask(std::uint64_t group_id, std::uint64_t seq) {
        auto probe = dstd::make_shared<Probe>();
        std::uint64_t nonce = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = members_.find(group_id);
            if (found == members_.end()) return {};
            probe->members = found->second;
            nonce = ++next_nonce_;
            probes_[nonce] = probe;
        }
        const std::uint32_t me = context_->rank();
        probe->mask.assign(probe->members.size(), 0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto reached = reached_.find(group_id);
            const bool here =
                reached != reached_.end() && reached->second > seq;
            for (std::size_t at = 0; at < probe->members.size(); ++at) {
                if (probe->members[at] == me)
                    probe->mask[at] = here ? 1 : 0;
                else
                    ++probe->waiting;
            }
        }
        if (probe->waiting > 0) {
            context_->submit([self = shared_from_this(), probe, nonce, group_id,
                              seq]() mutable {
                for (const std::uint32_t member : probe->members) {
                    if (member == self->context_->rank()) continue;
                    auto query = dstd::make_shared<Query>();
                    query->nonce = nonce;
                    query->group = group_id;
                    query->seq = seq;
                    query->requester = self->context_->rank();
                    MPI_Request request = MPI_REQUEST_NULL;
                    MPI_Isend(query.get(), sizeof(Query), MPI_BYTE,
                              static_cast<int>(member), TAG_QUERY,
                              self->context_->messages(), &request);
                    self->context_->watch(request, [query] {});
                }
            });
            std::unique_lock<std::mutex> lock(probe->mutex);
            probe->wake.wait_for(
                lock,
                std::chrono::milliseconds(
                    common::constants::comm_settings::MPI_ARRIVAL_PROBE_MS),
                [&probe] { return probe->waiting == 0; });
        }
        backend::Arrival mask;
        {
            std::lock_guard<std::mutex> lock(probe->mutex);
            mask = probe->mask;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        probes_.erase(nonce);
        return mask;
    }

    /// Answers the arrival questions of other members and takes the answers to
    /// ours, on the progress thread.
    bool poll() {
        bool progressed = false;
        for (;;) {
            int flag = 0;
            MPI_Status status;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_QUERY, context_->messages(), &flag,
                       &status);
            if (!flag) break;
            progressed = true;
            Query query;
            MPI_Recv(&query, sizeof(Query), MPI_BYTE, status.MPI_SOURCE,
                     TAG_QUERY, context_->messages(), MPI_STATUS_IGNORE);
            auto reply = dstd::make_shared<Reply>();
            reply->nonce = query.nonce;
            reply->responder = context_->rank();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto reached = reached_.find(query.group);
                reply->reached =
                    reached != reached_.end() && reached->second > query.seq
                        ? 1
                        : 0;
            }
            MPI_Request request = MPI_REQUEST_NULL;
            MPI_Isend(reply.get(), sizeof(Reply), MPI_BYTE, status.MPI_SOURCE,
                      TAG_REPLY, context_->messages(), &request);
            context_->watch(request, [reply] {});
        }
        for (;;) {
            int flag = 0;
            MPI_Status status;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_REPLY, context_->messages(), &flag,
                       &status);
            if (!flag) break;
            progressed = true;
            Reply reply;
            MPI_Recv(&reply, sizeof(Reply), MPI_BYTE, status.MPI_SOURCE,
                     TAG_REPLY, context_->messages(), MPI_STATUS_IGNORE);
            dstd::SharedPtr<Probe> probe;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto at = probes_.find(reply.nonce);
                if (at != probes_.end()) probe = at->second;
            }
            if (!probe) continue;
            std::lock_guard<std::mutex> lock(probe->mutex);
            for (std::size_t at = 0; at < probe->members.size(); ++at) {
                if (probe->members[at] != reply.responder) continue;
                probe->mask[at] = reply.reached != 0 ? 1 : 0;
                if (probe->waiting > 0) --probe->waiting;
            }
            probe->wake.notify_all();
        }
        return progressed;
    }

    dstd::SharedPtr<Context> context_;
    std::uint64_t poller_ = 0;
    std::atomic<std::uint64_t> by_kind_[KINDS] = {};
    std::atomic<std::uint64_t> folds_{0};
    std::atomic<std::uint64_t> limit_{0};
    std::mutex mutex_;
    dstd::Map<SlotKey, SlotPtr> slots_;
    dstd::Map<std::uint64_t, std::uint64_t> reached_;
    dstd::Map<std::uint64_t, dstd::Vector<std::uint32_t>> members_;
    dstd::Map<std::uint64_t, dstd::SharedPtr<Probe>> probes_;
    std::uint64_t next_nonce_ = 0;
    /// The map is guarded by the mutex; a group's own state is touched on the
    /// progress thread only.
    dstd::Map<std::uint64_t, GroupPtr> groups_;
};

/// Stops the provider from taking questions when its last handle goes. The
/// state lives on while queued jobs hold it.
struct Detacher {
    explicit Detacher(dstd::SharedPtr<Impl> held) : impl(std::move(held)) {}
    Detacher(const Detacher&) = delete;
    Detacher& operator=(const Detacher&) = delete;
    ~Detacher() { impl->detach(); }
    dstd::SharedPtr<Impl> impl;
};

}  // namespace

dstd::SharedPtr<backend::Collectives> make_collectives(
    dstd::SharedPtr<Context> context) {
    auto impl = dstd::make_shared<Impl>(std::move(context));
    impl->attach();
    auto detacher = dstd::make_shared<Detacher>(impl);
    return dstd::SharedPtr<backend::Collectives>(detacher, impl.get());
}

}  // namespace dftracer::utils::comm::mpi
