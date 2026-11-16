// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/mpi/mpi_backend.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::mpi {

namespace {

using backend::MessageKey;
using backend::OpId;
using backend::Payload;
using backend::StreamOpts;

constexpr int TAG_HEADER = 1;
constexpr int TAG_PAYLOAD = 2;
constexpr std::size_t MAX_CHUNK_BYTES = std::size_t{1} << 30;

enum class Kind : std::uint32_t {
    Data,
    Ack,
    Close,
    Cancel,
    CancelReply,
    Abort
};

/// The fixed header of every message between processes. A payload follows it
/// in messages of at most CHUNK_BYTES each, on the payload tag; an Ack and an
/// Abort carry the text of their error as payload. The source and destination
/// are group ranks, which name the stream; origin is the world rank of the
/// sender, where a reply goes.
struct Wire {
    std::uint32_t kind = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint32_t overflow = 0;
    std::uint64_t group = 0;
    std::uint64_t tag = 0;
    std::uint64_t depth = 0;
    std::uint64_t op = 0;
    std::uint64_t payload = 0;
    /// For an error: whether it has one, its comm code or -1, its condition.
    std::int32_t has_error = 0;
    std::int32_t code = -1;
    std::uint32_t condition = 0;
    /// For a CancelReply: 1 when the receiver held the message.
    std::uint32_t won = 0;
    std::uint32_t origin = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(Wire) == 80);

/// What stays alive until MPI finished with a message: the header and the
/// bytes it borrows.
struct Outgoing {
    Wire wire;
    Payload payload;
    /// The world rank the message goes to.
    std::uint32_t to = 0;
};

dstd::Error make_peer_error(const Wire& wire, dstd::StringView text) {
    if (wire.code >= 0)
        return dstd::Error(static_cast<ChannelError>(wire.code), text);
    return dstd::Error(static_cast<dstd::Condition>(wire.condition), text);
}

void fill_error(Wire& wire, const dstd::Error* why) {
    wire.has_error = why != nullptr;
    if (why == nullptr) return;
    wire.condition = static_cast<std::uint32_t>(why->condition());
    wire.code = why->in_domain(COMM_ERROR_DOMAIN) ? why->code() : -1;
}

struct PendingSend {
    MessageKey key;
    backend::Messaging::SendDone done;
    /// Set once the sender asked the receiver to cancel; used only if the
    /// receiver says it won.
    dstd::Optional<dstd::Error> cancel_reason;
};

struct RemoteKey {
    std::uint32_t source;
    std::uint64_t op;
    friend bool operator<(const RemoteKey& a, const RemoteKey& b) noexcept {
        if (a.source != b.source) return a.source < b.source;
        return a.op < b.op;
    }
};

struct RemoteOp {
    MessageKey key;
    OpId id;
    bool done = false;
};

class Impl : public backend::Messaging,
             public std::enable_shared_from_this<Impl> {
   public:
    Impl(dstd::SharedPtr<Context> context, std::size_t chunk_bytes)
        : context_(std::move(context)),
          engine_(backend::make_local_messaging()),
          rank_(context_->rank()),
          chunk_bytes_(chunk_bytes) {}

    /// Starts taking the messages of the other processes. Not in the
    /// constructor, which cannot name the shared owner.
    void attach() {
        poller_ = context_->add_poller(
            [self = shared_from_this()] { return self->poll(); });
    }

    /// Stops taking messages; the jobs already queued keep the state alive
    /// until they ran.
    void detach() { context_->remove_poller(poller_); }

    OpId send(const MessageKey& key, Payload payload, StreamOpts opts,
              SendDone done) override {
        const std::uint32_t to = world_of(key.group, key.destination);
        if (to == rank_)
            return engine_->send(key, std::move(payload), opts,
                                 std::move(done));
        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = ++next_op_;
            pending_[id] = PendingSend{key, std::move(done), {}};
        }
        auto out = dstd::make_shared<Outgoing>();
        out->to = to;
        out->wire.kind = static_cast<std::uint32_t>(Kind::Data);
        out->wire.source = key.source;
        out->wire.destination = key.destination;
        out->wire.group = key.group;
        out->wire.tag = key.tag;
        out->wire.depth = opts.depth;
        out->wire.overflow = static_cast<std::uint32_t>(opts.overflow);
        out->wire.op = id;
        out->wire.payload = payload.size();
        out->payload = std::move(payload);
        transmit(std::move(out));
        return OpId{id};
    }

    OpId recv(const MessageKey& key, RecvDone done) override {
        return engine_->recv(key, std::move(done));
    }

    OpId recv_any_source(const MessageKey& key, std::uint32_t senders,
                         RecvFromDone done) override {
        return engine_->recv_any_source(key, senders, std::move(done));
    }

    OpId recv_into(const MessageKey& key, std::uint32_t senders,
                   dstd::Span<std::byte> buffer, RecvIntoDone done) override {
        return engine_->recv_into(key, senders, buffer, std::move(done));
    }

    bool cancel(const MessageKey& key, OpId op, dstd::Error why) override {
        const std::uint32_t to = world_of(key.group, key.destination);
        if (to == rank_) return engine_->cancel(key, op, why);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto at = pending_.find(op.value);
            if (at == pending_.end()) return false;
            at->second.cancel_reason = std::move(why);
        }
        auto out = dstd::make_shared<Outgoing>();
        out->to = to;
        out->wire.kind = static_cast<std::uint32_t>(Kind::Cancel);
        out->wire.source = key.source;
        out->wire.destination = key.destination;
        out->wire.group = key.group;
        out->wire.tag = key.tag;
        out->wire.op = op.value;
        transmit(std::move(out));
        return true;
    }

    void close(const MessageKey& key) override {
        const std::uint32_t to = world_of(key.group, key.destination);
        if (to == rank_) {
            engine_->close(key);
            return;
        }
        auto out = dstd::make_shared<Outgoing>();
        out->to = to;
        out->wire.kind = static_cast<std::uint32_t>(Kind::Close);
        out->wire.source = key.source;
        out->wire.destination = key.destination;
        out->wire.group = key.group;
        out->wire.tag = key.tag;
        transmit(std::move(out));
    }

    void abort(std::uint64_t group, dstd::Error why) override {
        engine_->abort(group, why);
        dstd::Vector<PendingSend> failed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto at = pending_.begin(); at != pending_.end();) {
                if (at->second.key.group != group) {
                    ++at;
                    continue;
                }
                failed.push_back(std::move(at->second));
                at = pending_.erase(at);
            }
        }
        for (PendingSend& send : failed)
            if (send.done) send.done(&why);
    }

    void on_peer_abort(PeerAbort handler) override {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_abort_ = dstd::make_shared<PeerAbort>(std::move(handler));
    }

    void announce_abort(std::uint64_t group, std::uint64_t parent,
                        const dstd::Error& why) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!announced_.insert(group).second) return;
        }
        const dstd::StringView text = why.message();
        dstd::Vector<std::uint32_t> peers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at = groups_.find(group);
            if (at != groups_.end()) {
                peers = at->second;
            } else {
                for (std::uint32_t peer = 0; peer < context_->size(); ++peer)
                    peers.push_back(peer);
            }
        }
        for (const std::uint32_t peer : peers) {
            if (peer == rank_) continue;
            auto out = dstd::make_shared<Outgoing>();
            out->to = peer;
            out->wire.kind = static_cast<std::uint32_t>(Kind::Abort);
            out->wire.source = rank_;
            out->wire.destination = peer;
            out->wire.group = group;
            out->wire.tag = parent;
            fill_error(out->wire, &why);
            dstd::Vector<std::byte> bytes(text.size());
            if (!text.empty())
                std::memcpy(bytes.data(), text.data(), text.size());
            out->wire.payload = bytes.size();
            out->payload = Payload(std::move(bytes));
            transmit(std::move(out));
        }
    }

    void bind_group(std::uint64_t group,
                    dstd::Span<const std::uint32_t> members) override {
        std::lock_guard<std::mutex> lock(mutex_);
        groups_[group].assign(members.begin(), members.end());
    }

    void release(std::uint64_t group) override {
        engine_->release(group);
        std::lock_guard<std::mutex> lock(mutex_);
        announced_.erase(group);
        groups_.erase(group);
    }

   private:
    /// The world rank of group rank @p rank of @p group; a group that was
    /// never bound names world ranks.
    std::uint32_t world_of(std::uint64_t group, std::uint32_t rank) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto at = groups_.find(group);
        if (at == groups_.end() || rank >= at->second.size()) return rank;
        return at->second[rank];
    }

    /// Hands the message to the progress thread, which posts its sends.
    void transmit(dstd::SharedPtr<Outgoing> message) {
        message->wire.origin = rank_;
        context_->submit([self = shared_from_this(),
                          out = std::move(message)]() mutable {
            const int dest = static_cast<int>(out->to);
            MPI_Request request = MPI_REQUEST_NULL;
            MPI_Isend(&out->wire, static_cast<int>(sizeof(Wire)), MPI_BYTE,
                      dest, TAG_HEADER, self->context_->messages(), &request);
            self->context_->watch(request, [out] {});
            const dstd::Span<const std::byte> bytes = out->payload.bytes();
            for (std::size_t at = 0; at < bytes.size();
                 at += self->chunk_bytes_) {
                const std::size_t count =
                    std::min(self->chunk_bytes_, bytes.size() - at);
                MPI_Isend(bytes.data() + at, static_cast<int>(count), MPI_BYTE,
                          dest, TAG_PAYLOAD, self->context_->messages(),
                          &request);
                self->context_->watch(request, [out] {});
            }
        });
    }

    /// Takes every header that arrived, and its payload, on the progress
    /// thread, and gives each to the completion thread.
    bool poll() {
        bool progressed = false;
        for (;;) {
            int flag = 0;
            MPI_Status status;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_HEADER, context_->messages(), &flag,
                       &status);
            if (!flag) break;
            progressed = true;
            Wire header;
            MPI_Recv(&header, static_cast<int>(sizeof(Wire)), MPI_BYTE,
                     status.MPI_SOURCE, TAG_HEADER, context_->messages(),
                     MPI_STATUS_IGNORE);
            const MessageKey key{header.group, header.source,
                                 header.destination, header.tag};
            const bool data = static_cast<Kind>(header.kind) == Kind::Data;
            // A receive into a caller's buffer takes the bytes straight from
            // the wire, unless an earlier message of its stream still waits
            // for the completion thread: that one must pair first.
            dstd::Optional<backend::Landing> landing;
            if (data && header.payload > 0 && !stream_busy(key))
                landing = engine_->claim_landing(key, header.payload);
            if (landing) {
                receive_payload(landing->buffer.data(), header.payload,
                                status.MPI_SOURCE);
                landed_.fetch_add(header.payload, std::memory_order_relaxed);
                context_->complete([self = shared_from_this(), header,
                                    taken = std::move(*landing)]() mutable {
                    taken.done(header.payload);
                    self->acknowledge(header.origin, header.op, nullptr);
                });
                continue;
            }
            dstd::Vector<std::byte> body(header.payload);
            receive_payload(body.data(), body.size(), status.MPI_SOURCE);
            if (data) hold_stream(key);
            context_->complete([self = shared_from_this(), header,
                                held = std::move(body)]() mutable {
                self->handle(header, std::move(held));
            });
        }
        return progressed;
    }

    std::uint64_t landed_bytes() const noexcept override {
        return landed_.load(std::memory_order_relaxed);
    }

    dstd::Optional<backend::Landing> claim_landing(const MessageKey& key,
                                                   std::size_t bytes) override {
        return engine_->claim_landing(key, bytes);
    }

    /// Receives @p total bytes of a payload from @p source into @p into. The
    /// sender cut the payload in pieces of its own size, none above
    /// MAX_CHUNK_BYTES, so each receive asks for all that is left.
    void receive_payload(std::byte* into, std::size_t total, int source) {
        for (std::size_t at = 0; at < total;) {
            const std::size_t room = std::min(MAX_CHUNK_BYTES, total - at);
            MPI_Status got;
            MPI_Recv(into + at, static_cast<int>(room), MPI_BYTE, source,
                     TAG_PAYLOAD, context_->messages(), &got);
            int count = 0;
            MPI_Get_count(&got, MPI_BYTE, &count);
            at += static_cast<std::size_t>(count);
        }
    }

    /// Whether messages of @p key wait for the completion thread.
    bool stream_busy(const MessageKey& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return held_streams_.find(key) != held_streams_.end();
    }

    void hold_stream(const MessageKey& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++held_streams_[key];
    }

    void release_stream(const MessageKey& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto at = held_streams_.find(key);
        if (at != held_streams_.end() && --at->second == 0)
            held_streams_.erase(at);
    }

    void handle(const Wire& wire, dstd::Vector<std::byte> bytes) {
        const MessageKey key{wire.group, wire.source, wire.destination,
                             wire.tag};
        switch (static_cast<Kind>(wire.kind)) {
            case Kind::Data:
                receive_data(wire, key, std::move(bytes));
                release_stream(key);
                return;
            case Kind::Ack: {
                PendingSend taken;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto at = pending_.find(wire.op);
                    if (at == pending_.end()) return;
                    taken = std::move(at->second);
                    pending_.erase(at);
                }
                if (!taken.done) return;
                if (wire.has_error == 0) {
                    taken.done(nullptr);
                } else {
                    const dstd::Error why = make_peer_error(
                        wire, dstd::StringView(
                                  reinterpret_cast<const char*>(bytes.data()),
                                  bytes.size()));
                    // Only the receiver's answer to our own cancel is replaced
                    // by our reason; any other failure of it is the real one.
                    const bool by_cancel = why.is_code(ChannelError::Cancelled);
                    taken.done(by_cancel && taken.cancel_reason
                                   ? &*taken.cancel_reason
                                   : &why);
                }
                return;
            }
            case Kind::Close:
                engine_->close(key);
                return;
            case Kind::Cancel: {
                RemoteOp op;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto at = remote_.find(RemoteKey{wire.origin, wire.op});
                    if (at != remote_.end()) {
                        op = at->second;
                        found = true;
                    }
                }
                const bool won =
                    found && engine_->cancel(
                                 op.key, op.id,
                                 dstd::Error(ChannelError::Cancelled,
                                             "the sender cancelled the send"));
                auto out = dstd::make_shared<Outgoing>();
                out->wire.kind = static_cast<std::uint32_t>(Kind::CancelReply);
                out->wire.source = rank_;
                out->wire.destination = wire.source;
                out->to = wire.origin;
                out->wire.op = wire.op;
                out->wire.won = won ? 1 : 0;
                transmit(std::move(out));
                return;
            }
            case Kind::CancelReply: {
                if (wire.won == 0) return;
                PendingSend taken;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto at = pending_.find(wire.op);
                    if (at == pending_.end()) return;
                    taken = std::move(at->second);
                    pending_.erase(at);
                }
                if (!taken.done) return;
                if (taken.cancel_reason)
                    taken.done(&*taken.cancel_reason);
                else
                    taken.done(nullptr);
                return;
            }
            case Kind::Abort: {
                const dstd::Error why = make_peer_error(
                    wire, dstd::StringView(
                              reinterpret_cast<const char*>(bytes.data()),
                              bytes.size()));
                dstd::SharedPtr<PeerAbort> handler;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    handler = peer_abort_;
                }
                if (handler)
                    (*handler)(wire.group, wire.tag, why);
                else
                    engine_->abort(wire.group, why);
                return;
            }
        }
    }

    void receive_data(const Wire& wire, const MessageKey& key,
                      dstd::Vector<std::byte> bytes) {
        const RemoteKey remote{wire.origin, wire.op};
        auto state = dstd::make_shared<RemoteOp>();
        state->key = key;
        const std::uint32_t source = wire.origin;
        const std::uint64_t op = wire.op;
        StreamOpts opts;
        opts.depth = wire.depth;
        opts.overflow = static_cast<Overflow>(wire.overflow);
        const OpId id = engine_->send(
            key, Payload(std::move(bytes)), opts,
            [self = shared_from_this(), state, remote, source,
             op](const dstd::Error* why) {
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    state->done = true;
                    self->remote_.erase(remote);
                }
                self->acknowledge(source, op, why);
            });
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state->done && id.value != backend::NO_OP.value) {
            state->id = id;
            remote_[remote] = *state;
        }
    }

    void acknowledge(std::uint32_t source, std::uint64_t op,
                     const dstd::Error* why) {
        auto out = dstd::make_shared<Outgoing>();
        out->wire.kind = static_cast<std::uint32_t>(Kind::Ack);
        out->wire.source = rank_;
        out->wire.destination = source;
        out->to = source;
        out->wire.op = op;
        fill_error(out->wire, why);
        if (why != nullptr) {
            const dstd::StringView text = why->message();
            dstd::Vector<std::byte> bytes(text.size());
            if (!text.empty())
                std::memcpy(bytes.data(), text.data(), text.size());
            out->wire.payload = bytes.size();
            out->payload = Payload(std::move(bytes));
        }
        transmit(std::move(out));
    }

    dstd::SharedPtr<Context> context_;
    dstd::SharedPtr<backend::Messaging> engine_;
    std::uint32_t rank_;
    std::size_t chunk_bytes_;
    std::uint64_t poller_ = 0;
    std::mutex mutex_;
    std::uint64_t next_op_ = 0;
    dstd::Map<std::uint64_t, PendingSend> pending_;
    dstd::Map<RemoteKey, RemoteOp> remote_;
    /// How many messages of each stream the completion thread has not paired
    /// yet.
    dstd::Map<MessageKey, std::size_t> held_streams_;
    std::atomic<std::uint64_t> landed_{0};
    dstd::Map<std::uint64_t, dstd::Vector<std::uint32_t>> groups_;
    dstd::Set<std::uint64_t> announced_;
    dstd::SharedPtr<PeerAbort> peer_abort_;
};

/// What stops a messaging from taking messages when its last handle goes. The
/// state itself lives on while queued jobs hold it.
struct Detacher {
    explicit Detacher(dstd::SharedPtr<Impl> held) : impl(std::move(held)) {}
    Detacher(const Detacher&) = delete;
    Detacher& operator=(const Detacher&) = delete;
    ~Detacher() { impl->detach(); }
    dstd::SharedPtr<Impl> impl;
};

}  // namespace

dstd::SharedPtr<backend::Messaging> make_messaging(
    dstd::SharedPtr<Context> context) {
    std::uint64_t chunk =
        common::Env::get<std::uint64_t>(
            common::constants::comm_settings::MPI_CHUNK_BYTES_ENV)
            .value_or(common::constants::comm_settings::MPI_CHUNK_BYTES);
    chunk = std::min<std::uint64_t>(std::max<std::uint64_t>(chunk, 1),
                                    MAX_CHUNK_BYTES);
    auto impl = dstd::make_shared<Impl>(std::move(context),
                                        static_cast<std::size_t>(chunk));
    impl->attach();
    auto detacher = dstd::make_shared<Detacher>(impl);
    return dstd::SharedPtr<backend::Messaging>(detacher, impl.get());
}

}  // namespace dftracer::utils::comm::mpi
