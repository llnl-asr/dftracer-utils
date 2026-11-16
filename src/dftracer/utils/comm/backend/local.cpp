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
#include <cstring>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::backend {

namespace {

using dstd::Error;

Error full_error(std::size_t depth, std::size_t size) {
    return Error::from_format<
        "the stream holds its {} messages and the {} bytes of the message do "
        "not fit">(ChannelError::Full, depth, size);
}

Error too_big_error(std::size_t buffer, std::size_t message) {
    return Error::from_format<
        "a buffer of {} bytes holds no message of {} "
        "bytes">(ChannelError::TooBig, buffer, message);
}

Error closed_error() {
    return Error::from_format<"the sender closed the stream">(
        ChannelError::Closed);
}

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
    OpId send(const MessageKey& key, dstd::Vector<std::byte> bytes,
              StreamOpts opts, SendDone done) override {
        Deliveries out;
        OpId id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const dstd::SharedPtr<const Error> why = ended(key.group);
            if (why) {
                out.push_back(send_failed(std::move(done), why));
            } else if (const auto at = streams_.find(key);
                       at != streams_.end() && at->second.closed) {
                out.push_back(send_failed(std::move(done), closed_error()));
            } else {
                Stream& stream = streams_[key];
                stream.depth = opts.depth;
                stream.overflow = opts.overflow;
                Sent message{OpId{next_op_++}, std::move(bytes),
                             std::move(done)};
                // A receive of any source takes the message before the receives
                // of the stream it came from.
                const MessageKey any_stream = any_key(key);
                const auto any = streams_.find(any_stream);
                bool taken = false;
                if (any != streams_.end() && !any->second.recvs.empty()) {
                    taken = pair(any->second, message, key.source, out);
                    sweep(any_stream);
                }
                if (!taken && !stream.recvs.empty())
                    taken = pair(stream, message, key.source, out);
                if (taken) {
                    sweep(key);
                } else {
                    const std::size_t depth = opts.depth;
                    if (depth == HOLD_ALL || stream.messages.size() < depth) {
                        accept(message, out);
                        stream.messages.push_back(std::move(message));
                    } else if (opts.overflow == Overflow::Block) {
                        id = message.op;
                        stream.held.push_back(std::move(message));
                    } else if (opts.overflow == Overflow::Fail) {
                        out.push_back(send_failed(
                            std::move(message.done),
                            full_error(depth, message.bytes.size())));
                    } else {
                        // The oldest message goes, its send completes as sent,
                        // and the new one takes its place.
                        Delivery dropped;
                        dropped.what = Delivery::What::Accepted;
                        dropped.message = std::move(stream.messages.front());
                        stream.messages.pop_front();
                        out.push_back(std::move(dropped));
                        accept(message, out);
                        stream.messages.push_back(std::move(message));
                    }
                }
            }
        }
        run(out);
        return id;
    }

    OpId recv(const MessageKey& key, RecvDone done) override {
        return take(key, 0, Kind::Plain, std::move(done), RecvFromDone{},
                    RecvIntoDone{});
    }

    OpId recv_any_source(const MessageKey& key, std::uint32_t senders,
                         RecvFromDone done) override {
        return take(key, senders, Kind::Any, RecvDone{}, std::move(done),
                    RecvIntoDone{});
    }

    OpId recv_into(const MessageKey& key, std::uint32_t senders,
                   dstd::Span<std::byte> buffer, RecvIntoDone done) override {
        return take(key, senders, Kind::Into, RecvDone{}, RecvFromDone{},
                    std::move(done), buffer);
    }

    bool cancel(const MessageKey& key, OpId op, Error why) override {
        if (op.value == 0) return false;
        Deliveries out;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at = streams_.find(key);
            if (at != streams_.end()) {
                Stream& stream = at->second;
                found = cancel_send(stream.held, op, why, out) ||
                        cancel_recv(stream.recvs, op, why, out);
            }
            sweep(key);
        }
        run(out);
        return found;
    }

    void close(const MessageKey& key) override {
        Deliveries out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ended(key.group)) return;
            Stream& stream = streams_[key];
            if (stream.closed) return;
            stream.closed = true;
            // What a full stream held goes behind the messages it waited for,
            // so a receive takes every message sent before the close.
            flush_held(stream, out);
            pump(stream, key.source, out);
            while (!stream.recvs.empty()) {
                out.push_back(recv_failed(std::move(stream.recvs.front()),
                                          closed_error()));
                stream.recvs.pop_front();
            }
            // A receive of any source answers once every sender it names
            // closed.
            const MessageKey any_stream = any_key(key);
            const auto any = streams_.find(any_stream);
            if (any != streams_.end() && !any->second.recvs.empty()) {
                const std::size_t closed_count = closed_senders(key);
                dstd::Deque<Recv>& recvs = any->second.recvs;
                for (auto at = recvs.begin(); at != recvs.end();) {
                    if (closed_count < at->senders) {
                        ++at;
                        continue;
                    }
                    out.push_back(recv_failed(std::move(*at), closed_error()));
                    at = recvs.erase(at);
                }
                sweep(any_stream);
            }
        }
        run(out);
    }

    void abort(std::uint64_t group, Error why) override {
        Deliveries out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // The first abort of a group keeps its error.
            if (ended_.find(group) == ended_.end())
                ended_[group] = dstd::make_shared<const Error>(why);
            end_group(group, false, out);
        }
        run(out);
    }

    void release(std::uint64_t group) override {
        Deliveries out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            end_group(group, true, out);
        }
        run(out);
    }

    LocalCounts counts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        LocalCounts out;
        out.streams = streams_.size();
        out.ended = ended_.size();
        for (const auto& entry : streams_) {
            out.parked += entry.second.messages.size() +
                          entry.second.held.size() + entry.second.recvs.size();
        }
        return out;
    }

   private:
    /// What a receive of a stream takes.
    enum class Kind : std::uint8_t { Plain, Any, Into };

    /// One message, waiting for a receive or held back by the depth of its
    /// stream.
    struct Sent {
        OpId op;
        dstd::Vector<std::byte> bytes;
        SendDone done;
    };

    /// One receive, waiting for a message of its stream.
    struct Recv {
        OpId op;
        Kind kind = Kind::Plain;
        /// The senders a receive of any source waits for to close.
        std::uint32_t senders = 0;
        dstd::Span<std::byte> buffer;
        RecvDone plain;
        RecvFromDone any;
        RecvIntoDone into;
    };

    /// The messages of one pair of ranks on one tag.
    struct Stream {
        /// The messages a receive takes, oldest first.
        dstd::Deque<Sent> messages;
        /// The sends a full stream holds until a receive frees it, oldest
        /// first.
        dstd::Deque<Sent> held;
        /// The receives waiting for a message, oldest first.
        dstd::Deque<Recv> recvs;
        /// Set by the close of the sender, which answers the later receives.
        bool closed = false;
        /// The depth the sender named, and what it does when the stream holds
        /// that much. A stream a receive made holds everything until a send
        /// names its own depth.
        std::size_t depth = HOLD_ALL;
        Overflow overflow = Overflow::Block;
    };

    /// What one call owes the ends of the calls it ended, run once the mutex is
    /// free.
    struct Delivery {
        enum class What : std::uint8_t {
            /// The message goes to the receive.
            Message,
            /// The buffer of the receive is too short, and the message stays.
            TooBig,
            /// The receive failed; its message, if any, stays.
            RecvFailed,
            /// The send failed.
            SendFailed,
            /// The send of a message the stream now holds: it was dropped by
            /// the overflow of the stream, or it waited for a full stream to
            /// free a slot.
            Accepted,
        };

        What what = What::Message;
        Sent message;
        Recv recv;
        std::uint32_t source = 0;
        dstd::SharedPtr<const Error> why;
    };

    /// What one call ended. The common call ends one, so one is kept inline and
    /// only a call that ends many, such as a close or an abort, allocates.
    class Deliveries {
       public:
        void push_back(Delivery delivery) {
            if (inline_count_ < INLINE) {
                inline_[inline_count_++] = std::move(delivery);
                return;
            }
            more_.push_back(std::move(delivery));
        }

        template <class F>
        void each(F&& run_one) {
            for (std::size_t at = 0; at < inline_count_; ++at)
                run_one(inline_[at]);
            for (Delivery& delivery : more_) run_one(delivery);
            inline_count_ = 0;
            more_.clear();
        }

       private:
        static constexpr std::size_t INLINE = 2;
        Delivery inline_[INLINE];
        std::size_t inline_count_ = 0;
        dstd::Vector<Delivery> more_;
    };

    /// The stream the next receive of a key takes.
    struct Ready {
        MessageKey key;
        Stream* stream = nullptr;
        std::uint32_t source = 0;
    };

    static Delivery send_failed(SendDone done,
                                const dstd::SharedPtr<const Error>& why) {
        Delivery delivery;
        delivery.what = Delivery::What::SendFailed;
        delivery.message.done = std::move(done);
        delivery.why = why;
        return delivery;
    }

    static Delivery send_failed(SendDone done, Error why) {
        return send_failed(std::move(done),
                           dstd::make_shared<const Error>(std::move(why)));
    }

    static Delivery recv_failed(Recv recv,
                                const dstd::SharedPtr<const Error>& why) {
        Delivery delivery;
        delivery.what = Delivery::What::RecvFailed;
        delivery.recv = std::move(recv);
        delivery.why = why;
        return delivery;
    }

    static Delivery recv_failed(Recv recv, Error why) {
        return recv_failed(std::move(recv),
                           dstd::make_shared<const Error>(std::move(why)));
    }

    /// A receive of @p kind of @p key: of a message a sender parked, or of the
    /// next one.
    OpId take(const MessageKey& key, std::uint32_t senders, Kind kind,
              RecvDone plain, RecvFromDone any, RecvIntoDone into,
              dstd::Span<std::byte> buffer = {}) {
        Deliveries out;
        OpId id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const dstd::SharedPtr<const Error> why = ended(key.group);
            if (why) {
                Recv recv;
                recv.kind = kind;
                recv.plain = std::move(plain);
                recv.any = std::move(any);
                recv.into = std::move(into);
                out.push_back(recv_failed(std::move(recv), why));
            } else {
                const Ready ready = next_ready(key);
                if (ready.stream != nullptr) {
                    Sent& message = ready.stream->messages.front();
                    Delivery delivery;
                    delivery.source = ready.source;
                    delivery.recv.kind = kind;
                    delivery.recv.buffer = buffer;
                    delivery.recv.plain = std::move(plain);
                    delivery.recv.any = std::move(any);
                    delivery.recv.into = std::move(into);
                    if (kind == Kind::Into &&
                        message.bytes.size() > buffer.size()) {
                        delivery.what = Delivery::What::TooBig;
                        delivery.why = dstd::make_shared<const Error>(
                            too_big_error(buffer.size(), message.bytes.size()));
                        out.push_back(std::move(delivery));
                    } else {
                        delivery.what = Delivery::What::Message;
                        delivery.message = std::move(message);
                        ready.stream->messages.pop_front();
                        if (key.source == ANY_SOURCE)
                            served_[any_key(key)] = ready.source;
                        out.push_back(std::move(delivery));
                        // A slot is free, so the send the full stream held
                        // takes it and completes.
                        promote(*ready.stream, out);
                    }
                    sweep(ready.key);
                } else if (closed(key, senders)) {
                    Recv recv;
                    recv.kind = kind;
                    recv.plain = std::move(plain);
                    recv.any = std::move(any);
                    recv.into = std::move(into);
                    out.push_back(recv_failed(std::move(recv), closed_error()));
                } else {
                    id = OpId{next_op_++};
                    Recv recv;
                    recv.op = id;
                    recv.kind = kind;
                    recv.senders = senders;
                    recv.buffer = buffer;
                    recv.plain = std::move(plain);
                    recv.any = std::move(any);
                    recv.into = std::move(into);
                    streams_[key].recvs.push_back(std::move(recv));
                }
            }
        }
        run(out);
        return id;
    }

    void deliver(Delivery& delivery) {
        const Error* why = delivery.why.get();
        switch (delivery.what) {
            case Delivery::What::Message: {
                const dstd::Span<const std::byte> bytes(
                    delivery.message.bytes.data(),
                    delivery.message.bytes.size());
                switch (delivery.recv.kind) {
                    case Kind::Plain:
                        delivery.recv.plain(bytes, nullptr);
                        break;
                    case Kind::Any:
                        delivery.recv.any(delivery.source, bytes, nullptr);
                        break;
                    case Kind::Into:
                        std::memcpy(delivery.recv.buffer.data(), bytes.data(),
                                    bytes.size());
                        delivery.recv.into(delivery.source, bytes.size(),
                                           nullptr);
                        break;
                }
                if (delivery.message.done) delivery.message.done(nullptr);
                break;
            }
            case Delivery::What::TooBig:
                delivery.recv.into(delivery.source, 0, why);
                break;
            case Delivery::What::RecvFailed:
                switch (delivery.recv.kind) {
                    case Kind::Plain:
                        delivery.recv.plain({}, why);
                        break;
                    case Kind::Any:
                        delivery.recv.any(0, {}, why);
                        break;
                    case Kind::Into:
                        delivery.recv.into(0, 0, why);
                        break;
                }
                break;
            case Delivery::What::SendFailed:
                if (delivery.message.done) delivery.message.done(why);
                break;
            case Delivery::What::Accepted:
                if (delivery.message.done) delivery.message.done(why);
                break;
        }
    }

    void run(Deliveries& out) {
        out.each([this](Delivery& delivery) { deliver(delivery); });
    }

    /// Gives @p message to the first receive of @p stream that can take it, and
    /// fails the ones whose buffer is too short, which leave it first in the
    /// stream. Gives false when no receive took it.
    bool pair(Stream& stream, Sent& message, std::uint32_t source,
              Deliveries& out) {
        while (!stream.recvs.empty()) {
            Recv taker = std::move(stream.recvs.front());
            stream.recvs.pop_front();
            if (taker.kind == Kind::Into &&
                message.bytes.size() > taker.buffer.size()) {
                Delivery delivery;
                delivery.what = Delivery::What::TooBig;
                delivery.source = source;
                delivery.recv = std::move(taker);
                delivery.why = dstd::make_shared<const Error>(too_big_error(
                    delivery.recv.buffer.size(), message.bytes.size()));
                out.push_back(std::move(delivery));
                continue;
            }
            Delivery delivery;
            delivery.what = Delivery::What::Message;
            delivery.source = source;
            delivery.recv = std::move(taker);
            delivery.message = std::move(message);
            out.push_back(std::move(delivery));
            return true;
        }
        return false;
    }

    /// Pairs the messages of @p stream with its receives, as far as a receive
    /// takes them.
    void pump(Stream& stream, std::uint32_t source, Deliveries& out) {
        while (!stream.messages.empty() && !stream.recvs.empty()) {
            Sent message = std::move(stream.messages.front());
            stream.messages.pop_front();
            if (!pair(stream, message, source, out)) {
                stream.messages.push_front(std::move(message));
                return;
            }
        }
    }

    /// Completes the send of @p message as sent once its stream holds it, and
    /// leaves the message with no completion for its receive to run.
    static void accept(Sent& message, Deliveries& out) {
        Delivery delivery;
        delivery.what = Delivery::What::Accepted;
        delivery.message.done = std::move(message.done);
        out.push_back(std::move(delivery));
    }

    /// Moves what a full stream held into it, as a receive frees a slot, and
    /// completes each of those sends as sent.
    void promote(Stream& stream, Deliveries& out) {
        while (!stream.held.empty() &&
               (stream.depth == HOLD_ALL ||
                stream.messages.size() < stream.depth)) {
            Sent message = std::move(stream.held.front());
            stream.held.pop_front();
            Delivery delivery;
            delivery.what = Delivery::What::Accepted;
            delivery.message.done = std::move(message.done);
            stream.messages.push_back(std::move(message));
            out.push_back(std::move(delivery));
        }
    }

    /// Moves every message a full stream held into it, for the close of it.
    void flush_held(Stream& stream, Deliveries& out) {
        const std::size_t depth = stream.depth;
        stream.depth = HOLD_ALL;
        promote(stream, out);
        stream.depth = depth;
    }

    /// The stream the next receive of @p key takes: the named one, or for a
    /// receive of any source the first sender after the one served last that
    /// parked a message, so no sender starves the others.
    Ready next_ready(const MessageKey& key) {
        Ready ready;
        ready.key = key;
        if (key.source != ANY_SOURCE) {
            const auto at = streams_.find(key);
            if (at == streams_.end() || at->second.messages.empty())
                return ready;
            ready.stream = &at->second;
            ready.source = key.source;
            return ready;
        }
        // The streams of one destination and tag are together, in order of
        // source: the first one past the served source is the next in turn,
        // and the first one overall is the lowest.
        const auto served = served_.find(key);
        const std::uint32_t from =
            served != served_.end() ? served->second + 1 : 0;
        const auto first = streams_.lower_bound(
            MessageKey{key.group, 0, key.destination, key.tag});
        const auto start = streams_.lower_bound(
            MessageKey{key.group, from, key.destination, key.tag});
        for (auto at = start; at != streams_.end(); ++at) {
            if (!same_senders(at->first, key)) break;
            if (at->first.source == ANY_SOURCE) break;
            if (at->second.messages.empty()) continue;
            ready.key = at->first;
            ready.stream = &at->second;
            ready.source = at->first.source;
            return ready;
        }
        for (auto at = first; at != start; ++at) {
            if (at->second.messages.empty()) continue;
            ready.key = at->first;
            ready.stream = &at->second;
            ready.source = at->first.source;
            return ready;
        }
        return ready;
    }

    static bool same_senders(const MessageKey& a,
                             const MessageKey& b) noexcept {
        return a.group == b.group && a.destination == b.destination &&
               a.tag == b.tag;
    }

    /// Whether a receive of @p key finds the sender of it closed. A receive of
    /// any source waits for @p senders senders to close.
    bool closed(const MessageKey& key, std::uint32_t senders) {
        if (key.source != ANY_SOURCE) {
            const auto at = streams_.find(key);
            return at != streams_.end() && at->second.closed;
        }
        return closed_senders(key) >= senders;
    }

    /// How many senders to the destination and tag of @p key closed their
    /// stream.
    std::size_t closed_senders(const MessageKey& key) const {
        std::size_t count = 0;
        for (auto at = streams_.lower_bound(
                 MessageKey{key.group, 0, key.destination, key.tag});
             at != streams_.end() && same_senders(at->first, key); ++at) {
            if (at->first.source == ANY_SOURCE) break;
            if (at->second.closed) ++count;
        }
        return count;
    }

    /// The stream of @p key that a receive of any source waits on.
    static MessageKey any_key(const MessageKey& key) noexcept {
        MessageKey any = key;
        any.source = ANY_SOURCE;
        return any;
    }

    /// The error of the end of @p group, or nothing when it did not end.
    dstd::SharedPtr<const Error> ended(std::uint64_t group) const {
        const auto at = ended_.find(group);
        return at != ended_.end() ? at->second : dstd::SharedPtr<const Error>{};
    }

    /// Fails every parked call of @p group with the error of its end, and
    /// forgets its streams. Forgets the end itself for @p forget.
    void end_group(std::uint64_t group, bool forget, Deliveries& out) {
        dstd::SharedPtr<const Error> why = ended(group);
        if (!why)
            why = dstd::make_shared<const Error>(
                Error::from_format<"the group ended">(
                    dstd::Condition::Cancelled));
        for (auto at = streams_.begin(); at != streams_.end();) {
            if (at->first.group != group) {
                ++at;
                continue;
            }
            Stream& stream = at->second;
            for (Sent& message : stream.messages)
                out.push_back(send_failed(std::move(message.done), why));
            for (Sent& message : stream.held)
                out.push_back(send_failed(std::move(message.done), why));
            for (Recv& recv : stream.recvs)
                out.push_back(recv_failed(std::move(recv), why));
            at = streams_.erase(at);
        }
        for (auto at = served_.begin(); at != served_.end();) {
            if (at->first.group == group)
                at = served_.erase(at);
            else
                ++at;
        }
        if (forget) ended_.erase(group);
    }

    bool cancel_send(dstd::Deque<Sent>& sends, OpId op, const Error& why,
                     Deliveries& out) {
        for (auto at = sends.begin(); at != sends.end(); ++at) {
            if (at->op.value != op.value) continue;
            Delivery delivery;
            delivery.what = Delivery::What::SendFailed;
            delivery.message = std::move(*at);
            delivery.why = dstd::make_shared<const Error>(why);
            sends.erase(at);
            out.push_back(std::move(delivery));
            return true;
        }
        return false;
    }

    bool cancel_recv(dstd::Deque<Recv>& recvs, OpId op, const Error& why,
                     Deliveries& out) {
        for (auto at = recvs.begin(); at != recvs.end(); ++at) {
            if (at->op.value != op.value) continue;
            Delivery delivery;
            delivery.what = Delivery::What::RecvFailed;
            delivery.recv = std::move(*at);
            delivery.why = dstd::make_shared<const Error>(why);
            recvs.erase(at);
            out.push_back(std::move(delivery));
            return true;
        }
        return false;
    }

    /// Drops @p key when nothing of it is left and its sender did not close it.
    void sweep(const MessageKey& key) {
        const auto at = streams_.find(key);
        if (at == streams_.end()) return;
        const Stream& stream = at->second;
        if (stream.closed) return;
        if (!stream.messages.empty() || !stream.held.empty() ||
            !stream.recvs.empty())
            return;
        streams_.erase(at);
    }

    mutable std::mutex mutex_;
    dstd::Map<MessageKey, Stream> streams_;
    /// The source a receive of any source served last, by its key.
    dstd::Map<MessageKey, std::uint32_t> served_;
    /// The error that ended each group, which every later call of it takes.
    dstd::Map<std::uint64_t, dstd::SharedPtr<const Error>> ended_;
    /// The id the next parked call takes, under @ref mutex_.
    std::uint64_t next_op_ = 1;
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
    LocalSlot(std::uint32_t member_count, Collectives::Done on_done)
        : records(member_count),
          seen(member_count, 0),
          members(member_count),
          done(std::move(on_done)) {}

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
