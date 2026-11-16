// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/backend/bulk_support.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <utility>

namespace dftracer::utils::comm::backend {

namespace {

using bulk_support::CreditGate;
using bulk_support::ExposureTable;
using bulk_support::Use;
using dstd::Condition;
using dstd::Error;

/// What the asking end of a transfer wants of the owner.
enum class Want : std::uint32_t { Read = 1, Write = 2, Cancel = 3 };

/// The one message that starts a transfer, on the request stream of the owner.
struct Request {
    std::uint32_t want = 0;
    /// A write whose bytes follow at once on the data stream, with no grant.
    std::uint32_t eager = 0;
    std::uint64_t xfer = 0;
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

static_assert(sizeof(Request) == 40);

/// What the owner sends back on the reply stream of a transfer: the answer to a
/// read, a grant for a range of a write, or its end. The text of an error
/// follows it.
struct Reply {
    std::uint32_t failed = 0;
    std::int32_t code = -1;
    std::uint32_t condition = 0;
    /// The last reply of the transfer, with no bytes behind it.
    std::uint32_t last = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

static_assert(sizeof(Reply) == 32);

template <class T>
dstd::Vector<std::byte> bytes_of(const T& value) {
    dstd::Vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

dstd::Vector<std::byte> failure_reply(const Error& why) {
    Reply reply;
    reply.failed = 1;
    reply.code = why.in_domain(COMM_ERROR_DOMAIN) ? why.code() : -1;
    reply.condition = static_cast<std::uint32_t>(why.condition());
    const dstd::StringView text = why.message();
    dstd::Vector<std::byte> bytes(sizeof(Reply) + text.size());
    std::memcpy(bytes.data(), &reply, sizeof(Reply));
    if (!text.empty())
        std::memcpy(bytes.data() + sizeof(Reply), text.data(), text.size());
    return bytes;
}

dstd::Vector<std::byte> ok_reply(std::uint64_t offset, std::uint64_t length,
                                 bool last) {
    Reply reply;
    reply.offset = offset;
    reply.length = length;
    reply.last = last ? 1 : 0;
    return bytes_of(reply);
}

/// Reads a reply: the error it carries in @p why, or none.
bool read_reply(dstd::Span<const std::byte> bytes, Reply& reply,
                dstd::Optional<Error>& why) {
    if (bytes.size() < sizeof(Reply)) return false;
    std::memcpy(&reply, bytes.data(), sizeof(Reply));
    if (reply.failed == 0) return true;
    const dstd::StringView text(
        reinterpret_cast<const char*>(bytes.data() + sizeof(Reply)),
        bytes.size() - sizeof(Reply));
    if (reply.code >= 0)
        why = Error(static_cast<ChannelError>(reply.code), text);
    else
        why = Error(static_cast<Condition>(reply.condition), text);
    return true;
}

Error protocol_error(std::uint32_t owner) {
    return Error::from_format<"rank {} sent a bulk message that is not one">(
        Condition::Internal, owner);
}

MessageKey request_key(std::uint64_t group, std::uint32_t from,
                       std::uint32_t to) {
    return MessageKey{group, from, to, message_tag(TagPurpose::BulkRequest, 0)};
}

MessageKey reply_key(std::uint64_t group, std::uint32_t owner,
                     std::uint32_t asker, std::uint64_t xfer) {
    return MessageKey{group, owner, asker,
                      message_tag(TagPurpose::BulkReply, xfer)};
}

MessageKey pull_data_key(std::uint64_t group, std::uint32_t owner,
                         std::uint32_t asker, std::uint64_t xfer) {
    return MessageKey{group, owner, asker,
                      message_tag(TagPurpose::BulkPullData, xfer)};
}

MessageKey data_key(std::uint64_t group, std::uint32_t pusher,
                    std::uint32_t owner, std::uint64_t xfer) {
    return MessageKey{group, pusher, owner,
                      message_tag(TagPurpose::BulkData, xfer)};
}

/// Runs a step again whenever it is asked for while it runs, one run at a time,
/// so a completion that comes back on the calling thread never nests another
/// run in the stack of the first.
class Trampoline {
   public:
    template <class Step>
    void run(Step&& step) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) {
                again_ = true;
                return;
            }
            running_ = true;
        }
        for (;;) {
            step();
            std::lock_guard<std::mutex> lock(mutex_);
            if (!again_) {
                running_ = false;
                return;
            }
            again_ = false;
        }
    }

   private:
    std::mutex mutex_;
    bool running_ = false;
    bool again_ = false;
};

class MessageBulk final : public Bulk,
                          public std::enable_shared_from_this<MessageBulk> {
   public:
    explicit MessageBulk(dstd::SharedPtr<Messaging> messaging)
        : messaging_(std::move(messaging)) {}

    dstd::Result<BufferRef> expose(std::uint64_t group, std::uint32_t rank,
                                   dstd::Span<std::byte> memory, Access access,
                                   MemoryKind kind) override {
        auto ref = table_.add(group, rank, memory, access, kind);
        if (ref) listen(group, rank);
        return ref;
    }

    void unexpose(const BufferRef& ref, Drained done) override {
        table_.close(ref, std::move(done));
    }

    OpId pull(std::uint32_t rank, const BufferRef& ref, std::uint64_t offset,
              dstd::Span<std::byte> dst, Done done) override {
        const std::uint64_t bytes = ref.size - std::min(offset, ref.size);
        return start(false, rank, ref, offset, dst.data(), nullptr, bytes,
                     std::move(done));
    }

    OpId push(std::uint32_t rank, const BufferRef& ref, std::uint64_t offset,
              dstd::Span<const std::byte> src, Done done) override {
        return start(true, rank, ref, offset, nullptr, src.data(), src.size(),
                     std::move(done));
    }

    bool cancel(std::uint64_t group, OpId op, Error why) override {
        dstd::SharedPtr<Client> client;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at = active_.find(op.value);
            if (at == active_.end() || at->second->group != group) return false;
            client = at->second;
        }
        client->stop(std::move(why));
        return true;
    }

    void abort(std::uint64_t group, Error why) override {
        table_.end(group, why);
        for (const auto& client : clients_of(group)) client->stop(why);
    }

    void bind_group(std::uint64_t group,
                    dstd::Span<const std::uint32_t> members) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sizes_[group] = static_cast<std::uint32_t>(members.size());
    }

    void release(std::uint64_t group) override {
        table_.release(group);
        const Error gone = Error::from_format<"group {} was released">(
            Condition::Cancelled, group);
        for (const auto& client : clients_of(group)) client->stop(gone);
        dstd::Vector<dstd::SharedPtr<Receiving>> receiving;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : receivings_)
                if (std::get<0>(entry.first) == group)
                    receiving.push_back(entry.second);
        }
        for (const auto& one : receiving) one->cancel();
        std::lock_guard<std::mutex> lock(mutex_);
        sizes_.erase(group);
        for (auto at = cancelled_writes_.begin();
             at != cancelled_writes_.end();) {
            if (std::get<0>(at->first) == group)
                at = cancelled_writes_.erase(at);
            else
                ++at;
        }
        for (auto at = listeners_.begin(); at != listeners_.end();) {
            if (at->first.first == group)
                at = listeners_.erase(at);
            else
                ++at;
        }
    }

    void limit_range_bytes(std::uint64_t bytes) noexcept override {
        range_.store(std::max<std::uint64_t>(bytes, 1),
                     std::memory_order_relaxed);
    }

    void limit_credit_bytes(std::uint64_t bytes) noexcept override {
        credit_.store(bytes, std::memory_order_relaxed);
    }

    std::uint64_t peak_in_flight() const noexcept override {
        return gate_.peak();
    }

    std::uint64_t credit_in_flight() const noexcept override {
        return gate_.in_flight();
    }

    dstd::SharedPtr<void> hold_credit(std::uint64_t group,
                                      std::uint32_t receiver,
                                      std::uint32_t peer,
                                      std::uint64_t bytes) override {
        return gate_.hold(CreditGate::Key{group, receiver, peer}, bytes,
                          credit_of(group));
    }

   private:
    struct Listener {
        Trampoline trampoline;
        std::atomic<bool> alive{true};
    };

    /// The asking end of one transfer: it sends the request, takes the ranges
    /// it needs, and ends the call. It holds no exposure; the owner holds its
    /// own for the transfer.
    ///
    /// The call ends only when none of its receives is parked any more, so the
    /// messaging never writes into the caller's buffer after the call ended,
    /// and (for a push) after the messaging released the caller's bytes. A
    /// transfer that ends early tells the owner of a push to stop, and takes
    /// the replies that are still on their way off its streams.
    class Client : public std::enable_shared_from_this<Client> {
       public:
        Client(dstd::SharedPtr<MessageBulk> bulk, bool is_write,
               std::uint32_t rank, const BufferRef& ref, std::uint64_t offset,
               std::byte* dst, const std::byte* src, std::uint64_t total,
               std::uint64_t xfer, Done done)
            : writes(is_write),
              group(ref.group),
              bulk_(std::move(bulk)),
              rank_(rank),
              ref_(ref),
              offset_(offset),
              dst_(dst),
              src_(src),
              total_(total),
              xfer_(xfer),
              eager_(total <=
                     common::constants::comm_settings::BULK_EAGER_BYTES),
              done_(std::move(done)) {}

        std::uint64_t id = 0;
        const bool writes;
        const std::uint64_t group;

        void begin() {
            trampoline_.run([self = shared_from_this()] { self->step(); });
        }

        /// Asks the transfer to end with @p why: now when it waits for credit,
        /// through its parked receives when it has some, and at its next step
        /// otherwise.
        void stop(Error why) {
            Stage stage;
            std::uint64_t ticket = 0;
            Parked parked[2];
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (finished_) return;
                if (!stopped_) stopped_ = why;
                stage = stage_;
                ticket = ticket_;
                for (int slot = 0; slot < 2; ++slot)
                    parked[slot] = parked_[slot];
            }
            if (stage == Stage::Credit) {
                if (bulk_->gate_.withdraw(gate_key(), ticket)) finish(why);
                return;
            }
            for (const Parked& one : parked)
                if (one.active)
                    (void)bulk_->messaging_->cancel(one.key, one.op, why);
        }

       private:
        enum class Stage : std::uint8_t { Idle, Credit, Message };

        /// A receive that this transfer parked in the messaging.
        struct Parked {
            MessageKey key;
            OpId op;
            bool active = false;
        };

        CreditGate::Key gate_key() const {
            return writes ? CreditGate::Key{group, ref_.owner, rank_}
                          : CreditGate::Key{group, rank_, ref_.owner};
        }

        dstd::Optional<Error> stop_reason() {
            std::lock_guard<std::mutex> lock(mutex_);
            return stopped_;
        }

        /// One turn of the transfer: the first, or the one after a range
        /// landed or a grant came.
        void step() {
            if (const auto why = stop_reason()) {
                finish(*why);
                return;
            }
            if (const auto ended = bulk_->table_.ended(group)) {
                finish(*ended);
                return;
            }
            if (writes)
                push_step();
            else
                pull_step();
        }

        void pull_step() {
            if (received_ == total_) {
                finish({});
                return;
            }
            const std::uint64_t remaining = total_ - received_;
            if (eager_) {
                range_ = remaining;
                ask();
                return;
            }
            const std::uint64_t credit = bulk_->credit_of(group);
            range_ = std::min({remaining, bulk_->range(), credit});
            std::unique_lock<std::mutex> lock(mutex_);
            stage_ = Stage::Credit;
            ++seq_;
            // Held across the call, so a grant that a release gives at once
            // waits here until the ticket is noted.
            const std::uint64_t at = bulk_->gate_.acquire(
                gate_key(), range_,
                [self = shared_from_this()] { self->ask(); }, credit);
            if (at != 0) {
                ticket_ = at;
                const bool cancelled = stopped_.has_value();
                lock.unlock();
                if (cancelled) stop(*stop_reason());
                return;
            }
            lock.unlock();
            ask();
        }

        /// Parks the receives of the next range, then sends the request for
        /// it: the bytes go straight into the caller's buffer.
        void ask() {
            if (const auto why = stop_reason()) {
                if (!eager_) bulk_->gate_.release(gate_key(), range_);
                finish(*why);
                return;
            }
            Request request;
            request.want = static_cast<std::uint32_t>(Want::Read);
            request.xfer = xfer_;
            request.id = ref_.id;
            request.offset = offset_ + received_;
            request.length = range_;
            const MessageKey answer =
                reply_key(group, ref_.owner, rank_, xfer_);
            const MessageKey data =
                pull_data_key(group, ref_.owner, rank_, xfer_);
            std::uint64_t seq = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stage_ = Stage::Message;
                seq = ++seq_;
                posting_ = true;
                credit_held_ = !eager_;
                range_open_ = true;
                header_done_ = false;
                data_done_ = false;
                header_failed_ = false;
                parked_[0] = Parked{};
                parked_[1] = Parked{};
                completed_[0] = completed_[1] = false;
            }
            const OpId header_op = bulk_->messaging_->recv(
                answer, [self = shared_from_this()](
                            dstd::Span<const std::byte> bytes,
                            const Error* why) { self->on_header(bytes, why); });
            note(seq, 0, answer, header_op);
            const OpId data_op = bulk_->messaging_->recv_into(
                data, 1, dstd::Span<std::byte>(dst_ + received_, range_),
                [self = shared_from_this()](std::uint32_t, std::size_t taken,
                                            const Error* why) {
                    self->on_data(taken, why);
                });
            note(seq, 1, data, data_op);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                posting_ = false;
            }
            send_request(request);
            settle();
        }

        void on_header(dstd::Span<const std::byte> bytes, const Error* why) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                completed_[0] = true;
                parked_[0].active = false;
            }
            if (why != nullptr) {
                stop(*why);
                settle();
                return;
            }
            Reply reply;
            dstd::Optional<Error> failure;
            if (!read_reply(bytes, reply, failure)) {
                stop(protocol_error(ref_.owner));
                settle();
                return;
            }
            if (failure) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    header_done_ = true;
                    header_failed_ = true;
                }
                // No bytes follow an error: the receive for them goes.
                stop(*failure);
                settle();
                return;
            }
            if (reply.length != range_) {
                stop(protocol_error(ref_.owner));
                settle();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                header_done_ = true;
            }
            advance();
        }

        void on_data(std::size_t taken, const Error* why) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                completed_[1] = true;
                parked_[1].active = false;
            }
            if (why != nullptr) {
                stop(*why);
                settle();
                return;
            }
            if (taken != range_) {
                stop(protocol_error(ref_.owner));
                settle();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                data_done_ = true;
            }
            advance();
        }

        /// Goes on to the next range once the header and the bytes of this one
        /// came.
        void advance() {
            std::uint64_t range = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!header_done_ || !data_done_) return;
                range = range_;
                range_open_ = false;
                if (credit_held_) {
                    credit_held_ = false;
                    bulk_->gate_.release(gate_key(), range);
                }
            }
            received_ += range;
            begin();
        }

        /// Ends the call once it was asked to stop and no receive is parked.
        void settle() {
            dstd::Optional<Error> why;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (finished_ || !stopped_ || posting_) return;
                for (int slot = 0; slot < 2; ++slot)
                    if (parked_[slot].active) return;
                why = stopped_;
            }
            if (why) finish(*why);
        }

        void push_step() {
            if (!requested_) {
                requested_ = true;
                Request request;
                request.want = static_cast<std::uint32_t>(Want::Write);
                request.eager = eager_ ? 1 : 0;
                request.xfer = xfer_;
                request.id = ref_.id;
                request.offset = offset_;
                request.length = total_;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    reply_open_ = true;
                }
                wait_grant();
                send_request(request);
                if (eager_)
                    send_data(dstd::Span<const std::byte>(src_, total_));
                return;
            }
            wait_grant();
        }

        void wait_grant() {
            const MessageKey key = reply_key(group, ref_.owner, rank_, xfer_);
            std::uint64_t seq = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stage_ = Stage::Message;
                seq = ++seq_;
                parked_[0] = Parked{};
                completed_[0] = false;
            }
            const OpId op = bulk_->messaging_->recv(
                key, [self = shared_from_this()](
                         dstd::Span<const std::byte> bytes, const Error* why) {
                    self->on_grant(bytes, why);
                });
            note(seq, 0, key, op);
        }

        void on_grant(dstd::Span<const std::byte> bytes, const Error* why) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                completed_[0] = true;
                parked_[0].active = false;
            }
            if (why != nullptr) {
                stop(*why);
                settle();
                return;
            }
            Reply reply;
            dstd::Optional<Error> failure;
            if (!read_reply(bytes, reply, failure)) {
                finish(protocol_error(ref_.owner));
                return;
            }
            if (failure || reply.last != 0) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    reply_open_ = false;
                }
                if (failure)
                    finish(*failure);
                else
                    finish({});
                return;
            }
            const std::uint64_t start = reply.offset - offset_;
            if (reply.offset < offset_ || start > total_ ||
                reply.length > total_ - start) {
                finish(protocol_error(ref_.owner));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++ranges_sent_;
            }
            send_data(dstd::Span<const std::byte>(src_ + start, reply.length));
            begin();
        }

        void send_request(const Request& request) {
            bulk_->messaging_->send(
                request_key(group, rank_, ref_.owner),
                Payload(bytes_of(request)), HOLD_EVERYTHING,
                [self = shared_from_this()](const Error* why) {
                    if (why != nullptr) {
                        self->stop(*why);
                        self->settle();
                    }
                });
        }

        /// Sends bytes of the caller's memory with no copy. The memory is in
        /// use until the message is released, which the borrow token counts.
        void send_data(dstd::Span<const std::byte> bytes) {
            if (bytes.empty()) return;
            bulk_->messaging_->send(
                data_key(group, rank_, ref_.owner, xfer_),
                Payload::borrowed(bytes, borrow()), HOLD_EVERYTHING,
                [self = shared_from_this()](const Error* why) {
                    if (why != nullptr) {
                        self->stop(*why);
                        self->settle();
                    }
                });
        }

        /// Remembers the receive in @p slot, so a stop reaches it, unless the
        /// receive ended or the transfer moved to another step meanwhile.
        void note(std::uint64_t seq, int slot, const MessageKey& key, OpId op) {
            dstd::Optional<Error> cancelled;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (seq_ != seq || completed_[slot] || op.value == 0) return;
                parked_[slot] = Parked{key, op, true};
                cancelled = stopped_;
            }
            if (cancelled) (void)bulk_->messaging_->cancel(key, op, *cancelled);
        }

        /// What keeps the call from ending while the messaging still reads the
        /// caller's memory: the call ends after the last copy is released.
        struct Borrow {
            explicit Borrow(dstd::SharedPtr<Client> held)
                : client(std::move(held)) {}
            Borrow(const Borrow&) = delete;
            Borrow& operator=(const Borrow&) = delete;
            ~Borrow() { client->borrow_dropped(); }
            dstd::SharedPtr<Client> client;
        };

        dstd::SharedPtr<const void> borrow() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++borrows_;
            }
            return dstd::make_shared<Borrow>(shared_from_this());
        }

        void borrow_dropped() {
            bool complete_now = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --borrows_;
                if (finished_ && borrows_ == 0 && !completed_call_) {
                    completed_call_ = true;
                    complete_now = true;
                }
            }
            if (complete_now) complete();
        }

        /// Ends the call once. The caller's memory is free to go when the
        /// call ends, so it ends after the messaging released it.
        void finish(const dstd::Optional<Error>& why) {
            bool complete_now = false;
            bool drain_header = false;
            bool drain_data = false;
            bool drain_replies = false;
            bool tell_owner = false;
            // The bytes already landed, so only the header is still to come.
            bool header_only = false;
            std::uint64_t sent = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (finished_) return;
                sent = ranges_sent_;
                finished_ = true;
                failure_ = why;
                if (why) {
                    if (!writes && range_open_) {
                        drain_header = !header_done_;
                        header_only = drain_header && data_done_;
                        drain_data =
                            header_done_ && !header_failed_ && !data_done_;
                    }
                    if (writes && reply_open_) {
                        drain_replies = true;
                        tell_owner = !eager_;
                    }
                }
                if (credit_held_) {
                    credit_held_ = false;
                    bulk_->gate_.release(gate_key(), range_);
                }
                if (borrows_ == 0) {
                    completed_call_ = true;
                    complete_now = true;
                }
            }
            bulk_->forget(id);
            if (why && !bulk_->table_.ended(group)) {
                if (tell_owner)
                    bulk_->send_cancel(group, rank_, ref_.owner, xfer_, sent);
                if (drain_header && header_only)
                    bulk_->drain(reply_key(group, ref_.owner, rank_, xfer_));
                else if (drain_header)
                    bulk_->drain_reply(group, ref_.owner, rank_, xfer_, true);
                if (drain_data)
                    bulk_->drain(
                        pull_data_key(group, ref_.owner, rank_, xfer_));
                if (drain_replies)
                    bulk_->drain_reply(group, ref_.owner, rank_, xfer_, false);
            }
            if (complete_now) complete();
        }

        void complete() {
            if (failure_)
                done_(0, &*failure_);
            else
                done_(total_, nullptr);
        }

        dstd::SharedPtr<MessageBulk> bulk_;
        const std::uint32_t rank_;
        const BufferRef ref_;
        const std::uint64_t offset_;
        std::byte* const dst_;
        const std::byte* const src_;
        const std::uint64_t total_;
        const std::uint64_t xfer_;
        const bool eager_;
        Done done_;
        Trampoline trampoline_;

        std::uint64_t received_ = 0;
        std::uint64_t range_ = 0;
        bool requested_ = false;

        std::mutex mutex_;
        Stage stage_ = Stage::Idle;
        std::uint64_t seq_ = 0;
        std::uint64_t ticket_ = 0;
        Parked parked_[2];
        bool completed_[2] = {false, false};
        bool posting_ = false;
        bool credit_held_ = false;
        bool range_open_ = false;
        bool header_done_ = false;
        bool header_failed_ = false;
        bool data_done_ = false;
        bool reply_open_ = false;
        std::uint64_t ranges_sent_ = 0;
        std::size_t borrows_ = 0;
        bool finished_ = false;
        bool completed_call_ = false;
        dstd::Optional<Error> stopped_;
        dstd::Optional<Error> failure_;
    };

    /// The receiving end of a write too large to send at once: the owner grants
    /// a range, takes it into the exposure, and grants the next, so the bytes
    /// in flight stay within the credit of the pusher. A cancel of the pusher
    /// ends it: it drops the parked receive, gives back its credit and its
    /// hold on the exposure, and grants nothing more.
    class Receiving : public std::enable_shared_from_this<Receiving> {
       public:
        Receiving(dstd::SharedPtr<MessageBulk> bulk, dstd::SharedPtr<Use> use,
                  std::uint64_t group, std::uint32_t owner,
                  std::uint32_t source, std::uint64_t xfer,
                  std::uint64_t offset, std::uint64_t length)
            : bulk_(std::move(bulk)),
              use_(std::move(use)),
              group_(group),
              owner_(owner),
              source_(source),
              xfer_(xfer),
              offset_(offset),
              length_(length) {}

        void begin() {
            trampoline_.run([self = shared_from_this()] { self->step(); });
        }

        /// The pusher gave up: end now when nothing is parked, and through the
        /// parked receive when something is.
        void cancel(std::uint64_t ranges_sent = 0) {
            std::uint64_t ticket = 0;
            bool queued = false;
            Parked parked;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ended_) return;
                cancelled_ = true;
                if (ranges_sent != 0) ranges_sent_ = ranges_sent;
                queued = queued_;
                ticket = ticket_;
                parked = parked_;
            }
            if (queued) {
                if (bulk_->gate_.withdraw(gate_key(), ticket))
                    end(cancel_error());
            } else if (parked.active) {
                (void)bulk_->messaging_->cancel(parked.key, parked.op,
                                                cancel_error());
            }
        }

       private:
        struct Parked {
            MessageKey key;
            OpId op;
            bool active = false;
        };

        CreditGate::Key gate_key() const {
            return CreditGate::Key{group_, owner_, source_};
        }

        void step() {
            if (is_cancelled()) {
                end(cancel_error());
                return;
            }
            if (const auto ended = bulk_->table_.ended(group_)) {
                end(*ended);
                return;
            }
            if (received_ == length_) {
                end({});
                return;
            }
            const std::uint64_t credit = bulk_->credit_of(group_);
            held_ = std::min({length_ - received_, bulk_->range(), credit});
            std::unique_lock<std::mutex> lock(mutex_);
            // Held across the call, so a grant that a release gives at once
            // waits here until the ticket is noted.
            const std::uint64_t at = bulk_->gate_.acquire(
                gate_key(), held_,
                [self = shared_from_this()] { self->grant(); }, credit);
            if (at != 0) {
                queued_ = true;
                ticket_ = at;
                const bool cancelled = cancelled_;
                lock.unlock();
                if (cancelled) cancel();
                return;
            }
            lock.unlock();
            grant();
        }

        void grant() {
            std::uint64_t seq = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queued_ = false;
                seq = ++seq_;
                parked_ = Parked{};
                recv_done_ = false;
                credit_held_ = true;
            }
            if (is_cancelled()) {
                end(cancel_error());
                return;
            }
            if (const auto ended = bulk_->table_.ended(group_)) {
                end(*ended);
                return;
            }
            const std::uint64_t at = offset_ + received_;
            const MessageKey data = data_key(group_, source_, owner_, xfer_);
            const OpId op = bulk_->messaging_->recv_into(
                data, 1, use_->memory().subspan(at, held_),
                [self = shared_from_this()](std::uint32_t, std::size_t taken,
                                            const Error* why) {
                    self->landed(taken, why);
                });
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (seq_ == seq && !recv_done_ && op.value != 0) {
                    parked_ = Parked{data, op, true};
                    cancelled = cancelled_;
                }
            }
            if (cancelled) cancel();
            bulk_->reply(reply_key(group_, owner_, source_, xfer_),
                         ok_reply(at, held_, false));
        }

        void landed(std::size_t taken, const Error* why) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                recv_done_ = true;
                parked_.active = false;
            }
            if (why != nullptr || taken != held_) {
                end(why != nullptr ? *why : protocol_error(source_));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                credit_held_ = false;
            }
            received_ += held_;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++ranges_landed_;
            }
            bulk_->gate_.release(gate_key(), held_);
            begin();
        }

        bool is_cancelled() {
            std::lock_guard<std::mutex> lock(mutex_);
            return cancelled_;
        }

        Error cancel_error() const {
            return Error::from_format<
                "the pusher cancelled its push of buffer {}">(
                Condition::Cancelled, xfer_);
        }

        /// Ends the write once: gives back the credit of a range in flight,
        /// tells the pusher how it ended, and lets go of the exposure.
        void end(const dstd::Optional<Error>& why) {
            bool give_back = false;
            std::uint64_t stale = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ended_) return;
                ended_ = true;
                give_back = credit_held_;
                credit_held_ = false;
                if (ranges_sent_ > ranges_landed_)
                    stale = ranges_sent_ - ranges_landed_;
            }
            if (give_back) bulk_->gate_.release(gate_key(), held_);
            // The ranges the pusher sent that no receive took are on their
            // way: take them off the stream, so the pusher's memory is free.
            for (; stale > 0; --stale)
                bulk_->drain(data_key(group_, source_, owner_, xfer_));
            bulk_->forget_receiving(group_, source_, xfer_);
            const MessageKey answer = reply_key(group_, owner_, source_, xfer_);
            if (why)
                bulk_->reply(answer, failure_reply(*why));
            else
                bulk_->reply(answer, ok_reply(offset_, length_, true));
            use_.reset();
        }

        dstd::SharedPtr<MessageBulk> bulk_;
        dstd::SharedPtr<Use> use_;
        const std::uint64_t group_;
        const std::uint32_t owner_;
        const std::uint32_t source_;
        const std::uint64_t xfer_;
        const std::uint64_t offset_;
        const std::uint64_t length_;
        std::uint64_t received_ = 0;
        std::uint64_t held_ = 0;
        Trampoline trampoline_;

        std::mutex mutex_;
        std::uint64_t seq_ = 0;
        std::uint64_t ticket_ = 0;
        Parked parked_;
        bool queued_ = false;
        bool recv_done_ = false;
        bool credit_held_ = false;
        bool cancelled_ = false;
        bool ended_ = false;
        std::uint64_t ranges_sent_ = 0;
        std::uint64_t ranges_landed_ = 0;
    };

    OpId start(bool writes, std::uint32_t rank, const BufferRef& ref,
               std::uint64_t offset, std::byte* dst, const std::byte* src,
               std::uint64_t bytes, Done done) {
        if (const auto ended = table_.ended(ref.group)) {
            done(0, &*ended);
            return NO_OP;
        }
        if (bytes == 0) {
            done(0, nullptr);
            return NO_OP;
        }
        auto client = dstd::make_shared<Client>(
            shared_from_this(), writes, rank, ref, offset, dst, src, bytes,
            next_xfer_.fetch_add(1, std::memory_order_relaxed),
            std::move(done));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client->id = next_op_++;
            active_[client->id] = client;
        }
        const OpId id{client->id};
        client->begin();
        return id;
    }

    void forget(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(id);
    }

    dstd::Vector<dstd::SharedPtr<Client>> clients_of(std::uint64_t group) {
        dstd::Vector<dstd::SharedPtr<Client>> found;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : active_)
            if (entry.second->group == group) found.push_back(entry.second);
        return found;
    }

    /// Answers the requests that peers send to @p rank, once, from the first
    /// exposure of that rank on.
    void listen(std::uint64_t group, std::uint32_t rank) {
        dstd::SharedPtr<Listener> listener;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& slot = listeners_[{group, rank}];
            if (slot) return;
            slot = dstd::make_shared<Listener>();
            listener = slot;
        }
        post_listen(group, rank, listener);
    }

    void post_listen(std::uint64_t group, std::uint32_t rank,
                     const dstd::SharedPtr<Listener>& listener) {
        listener->trampoline.run([this, group, rank, listener] {
            if (!listener->alive.load(std::memory_order_acquire)) return;
            const MessageKey key{group, ANY_SOURCE, rank,
                                 message_tag(TagPurpose::BulkRequest, 0)};
            // Weak: the parked receive sits in the messaging, which the
            // provider owns, so a strong hold would make a cycle.
            messaging_->recv_any_source(
                key, std::numeric_limits<std::uint32_t>::max(),
                [weak = weak_from_this(), group, rank, listener](
                    std::uint32_t source, dstd::Span<const std::byte> bytes,
                    const Error* why) {
                    const auto self = weak.lock();
                    if (!self) return;
                    if (why != nullptr) {
                        listener->alive.store(false, std::memory_order_release);
                        return;
                    }
                    Request request;
                    const bool whole = bytes.size() == sizeof(Request);
                    if (whole)
                        std::memcpy(&request, bytes.data(), sizeof(Request));
                    self->post_listen(group, rank, listener);
                    if (whole) self->serve(group, rank, source, request);
                });
        });
    }

    void forget_receiving(std::uint64_t group, std::uint32_t source,
                          std::uint64_t xfer) {
        std::lock_guard<std::mutex> lock(mutex_);
        receivings_.erase({group, source, xfer});
    }

    /// Tells the owner of a push that its asker gave up after it sent
    /// @p ranges_sent ranges.
    void send_cancel(std::uint64_t group, std::uint32_t rank,
                     std::uint32_t owner, std::uint64_t xfer,
                     std::uint64_t ranges_sent) {
        Request request;
        request.want = static_cast<std::uint32_t>(Want::Cancel);
        request.xfer = xfer;
        request.length = ranges_sent;
        messaging_->send(request_key(group, rank, owner),
                         Payload(bytes_of(request)), HOLD_EVERYTHING,
                         [](const Error*) {});
    }

    /// Takes one message of @p key off its stream and drops it.
    void drain(const MessageKey& key) {
        messaging_->recv(key, [](dstd::Span<const std::byte>, const Error*) {});
    }

    /// Takes the replies of a finished transfer off their stream: a pull has
    /// one, followed by its bytes when it is no error; a push has grants until
    /// the one that ends it.
    void drain_reply(std::uint64_t group, std::uint32_t owner,
                     std::uint32_t asker, std::uint64_t xfer, bool pull) {
        messaging_->recv(
            reply_key(group, owner, asker, xfer),
            [self = shared_from_this(), group, owner, asker, xfer, pull](
                dstd::Span<const std::byte> bytes, const Error* why) {
                if (why != nullptr) return;
                Reply reply;
                dstd::Optional<Error> failure;
                if (!read_reply(bytes, reply, failure) || failure) return;
                if (pull)
                    self->drain(pull_data_key(group, owner, asker, xfer));
                else if (reply.last == 0)
                    self->drain_reply(group, owner, asker, xfer, false);
            });
    }

    void reply(const MessageKey& key, dstd::Vector<std::byte> bytes) {
        messaging_->send(key, Payload(std::move(bytes)), HOLD_EVERYTHING,
                         [](const Error*) {});
    }

    void serve(std::uint64_t group, std::uint32_t owner, std::uint32_t source,
               const Request& request) {
        if (request.want == static_cast<std::uint32_t>(Want::Cancel)) {
            dstd::SharedPtr<Receiving> receiving;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto at = receivings_.find({group, source, request.xfer});
                if (at != receivings_.end())
                    receiving = at->second;
                else
                    // Its write is not here yet: it starts cancelled.
                    cancelled_writes_[{group, source, request.xfer}] =
                        request.length;
            }
            if (receiving) receiving->cancel(request.length);
            return;
        }
        const bool writes =
            request.want == static_cast<std::uint32_t>(Want::Write);
        const MessageKey answer = reply_key(group, owner, source, request.xfer);
        const MessageKey data = data_key(group, source, owner, request.xfer);
        const bool eager_write = writes && request.eager != 0;
        auto fail = [&](const Error& why) {
            reply(answer, failure_reply(why));
            // An eager write sent its bytes already; nothing will take them.
            if (eager_write)
                messaging_->recv(
                    data, [](dstd::Span<const std::byte>, const Error*) {});
        };
        if (request.want != static_cast<std::uint32_t>(Want::Read) && !writes)
            return fail(protocol_error(source));
        auto opened = table_.open(group, owner, request.id, writes);
        if (!opened) return fail(opened.error());
        const dstd::Span<std::byte> memory = (*opened)->memory();
        if (request.offset > memory.size() ||
            request.length > memory.size() - request.offset)
            return fail(Error::from_format<
                        "a transfer of {} bytes at offset {} does not fit "
                        "buffer {} of rank {}, which has {} bytes">(
                ChannelError::TooBig, request.length, request.offset,
                request.id, owner, memory.size()));
        if (!writes) {
            reply(answer, ok_reply(request.offset, request.length, true));
            messaging_->send(
                pull_data_key(group, owner, source, request.xfer),
                Payload::borrowed(
                    memory.subspan(request.offset, request.length), *opened),
                HOLD_EVERYTHING, [](const Error*) {});
            return;
        }
        if (eager_write) {
            messaging_->recv_into(
                data, 1, memory.subspan(request.offset, request.length),
                [self = shared_from_this(), use = *opened, answer,
                 length = request.length, offset = request.offset,
                 source](std::uint32_t, std::size_t taken, const Error* why) {
                    if (why != nullptr)
                        self->reply(answer, failure_reply(*why));
                    else if (taken != length)
                        self->reply(answer,
                                    failure_reply(protocol_error(source)));
                    else
                        self->reply(answer, ok_reply(offset, length, true));
                });
            return;
        }
        auto receiving = dstd::make_shared<Receiving>(
            shared_from_this(), std::move(*opened), group, owner, source,
            request.xfer, request.offset, request.length);
        bool cancelled = false;
        std::uint64_t stale = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at =
                cancelled_writes_.find({group, source, request.xfer});
            if (at != cancelled_writes_.end()) {
                cancelled = true;
                stale = at->second;
                cancelled_writes_.erase(at);
            } else {
                receivings_[{group, source, request.xfer}] = receiving;
            }
        }
        if (cancelled) {
            // The pusher gave up before this write came: grant nothing, let go
            // of the exposure, and take what it sent off the stream.
            for (; stale > 0; --stale) drain(data);
            reply(answer,
                  failure_reply(
                      Error::from_format<"the pusher cancelled its push of "
                                         "buffer {}">(Condition::Cancelled,
                                                      request.xfer)));
            return;
        }
        receiving->begin();
    }

    std::uint64_t range() const noexcept {
        return std::min<std::uint64_t>(
            range_.load(std::memory_order_relaxed),
            common::constants::comm_settings::BULK_RANGE_BYTES);
    }

    std::uint64_t credit_of(std::uint64_t group) {
        const std::uint64_t forced = credit_.load(std::memory_order_relaxed);
        if (forced != 0) return forced;
        std::uint32_t members = 2;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at = sizes_.find(group);
            if (at != sizes_.end()) members = at->second;
        }
        return bulk_support::credit_from_budget(members);
    }

    dstd::SharedPtr<Messaging> messaging_;
    ExposureTable table_;
    CreditGate gate_;
    std::mutex mutex_;
    std::uint64_t next_op_ = 1;
    std::atomic<std::uint64_t> next_xfer_{1};
    dstd::Map<std::uint64_t, dstd::SharedPtr<Client>> active_;
    dstd::Map<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>,
              dstd::SharedPtr<Receiving>>
        receivings_;
    /// Cancels that came before their write, with the ranges the pusher sent.
    dstd::Map<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>,
              std::uint64_t>
        cancelled_writes_;
    dstd::Map<std::uint64_t, std::uint32_t> sizes_;
    dstd::Map<std::pair<std::uint64_t, std::uint32_t>,
              dstd::SharedPtr<Listener>>
        listeners_;
    std::atomic<std::uint64_t> range_{
        common::constants::comm_settings::BULK_RANGE_BYTES};
    std::atomic<std::uint64_t> credit_{0};
};

}  // namespace

dstd::SharedPtr<Bulk> make_message_bulk(dstd::SharedPtr<Messaging> messaging) {
    return dstd::make_shared<MessageBulk>(std::move(messaging));
}

}  // namespace dftracer::utils::comm::backend
