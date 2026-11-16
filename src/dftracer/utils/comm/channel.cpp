// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/detail/channel_state.h>
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm {

using dstd::Condition;
using dstd::Error;

namespace detail {

namespace {

/// The index a channel takes in its tag space: the FNV-1a avalanche of its id,
/// so the tags of two channels share no bit pattern a mask would confuse.
std::uint64_t channel_index(std::uint64_t id) noexcept {
    return dstd::hash::fnv1a_mix(id);
}

}  // namespace

ChannelHub::ChannelHub(dstd::SharedPtr<backend::Messaging> messaging,
                       std::uint64_t group_id, std::uint32_t rank,
                       std::uint32_t size)
    : messaging_(std::move(messaging)),
      group_id_(group_id),
      rank_(rank),
      peers_(size) {}

void ChannelHub::note_open(std::uint64_t channel, std::uint32_t value_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    opened_[channel] = value_size;
}

ChannelHub::Open ChannelHub::open(std::uint32_t peer, std::uint64_t channel,
                                  ChannelOpen& wrong, std::uint64_t& seen) {
    std::lock_guard<std::mutex> lock(mutex_);
    seen = 0;
    if (peer >= peers_.size()) return Open::Waiting;
    const Peer& known = peers_[peer];
    seen = known.generation;
    const auto theirs = known.declared.find(channel);
    if (theirs == known.declared.end()) return Open::Waiting;
    const auto mine = opened_.find(channel);
    if (mine != opened_.end() && mine->second == theirs->second)
        return Open::Matched;
    wrong = ChannelOpen{channel, theirs->second, 0};
    return Open::Mismatch;
}

std::uint64_t ChannelHub::make_ticket() noexcept {
    return tickets_.fetch_add(1, std::memory_order_relaxed) + 1;
}

dstd::Vector<std::uint64_t> ChannelHub::unopened(std::uint32_t peer) {
    dstd::Vector<std::uint64_t> ids;
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer >= peers_.size()) return ids;
    for (const auto& entry : peers_[peer].declared)
        if (opened_.find(entry.first) == opened_.end())
            ids.push_back(entry.first);
    return ids;
}

bool ChannelHub::watch(std::uint32_t peer, std::uint64_t ticket,
                       const dstd::WeakPtr<PendingOp>& call, Wake wake,
                       std::uint64_t seen) {
    if (peer >= peers_.size()) return true;
    bool listen = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Peer& known = peers_[peer];
        if (known.generation != seen) return false;
        // A call that ended or is gone leaves its wait behind.
        for (std::size_t at = 0; at < known.watchers.size();) {
            const dstd::SharedPtr<PendingOp> waiting =
                known.watchers[at].call.lock();
            if (waiting && !waiting->is_done()) {
                ++at;
                continue;
            }
            known.watchers.erase(known.watchers.begin() +
                                 static_cast<std::ptrdiff_t>(at));
        }
        known.watchers.push_back(Watcher{ticket, call, std::move(wake)});
        listen = !known.listening;
        known.listening = true;
    }
    if (!listen) return true;
    const backend::MessageKey key{
        group_id_, peer, rank_,
        backend::message_tag(backend::TagPurpose::ChannelOpen, 0)};
    dstd::WeakPtr<ChannelHub> hub = this->weak_from_this();
    messaging_->recv(key, [hub, peer](dstd::Span<const std::byte> bytes,
                                      const dstd::Error* why) {
        if (const dstd::SharedPtr<ChannelHub> self = hub.lock())
            self->declared(peer, bytes, why);
    });
    return true;
}

bool ChannelHub::unwatch(std::uint32_t peer, std::uint64_t ticket) {
    if (peer >= peers_.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    dstd::Vector<Watcher>& watchers = peers_[peer].watchers;
    for (auto at = watchers.begin(); at != watchers.end(); ++at) {
        if (at->ticket != ticket) continue;
        watchers.erase(at);
        return true;
    }
    return false;
}

void ChannelHub::declared(std::uint32_t peer, dstd::Span<const std::byte> bytes,
                          const dstd::Error* why) {
    dstd::Error wrong_size;
    if (why == nullptr && bytes.size() != sizeof(ChannelOpen)) {
        wrong_size = Error::from_format<
            "a declaration holds {} bytes, not the {} bytes of an open">(
            Condition::InvalidArgument, bytes.size(), sizeof(ChannelOpen));
        why = &wrong_size;
    }
    dstd::Vector<Watcher> woken;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Peer& known = peers_[peer];
        known.listening = false;
        ++known.generation;
        if (why == nullptr) {
            ChannelOpen open;
            std::memcpy(&open, bytes.data(), sizeof(open));
            known.declared.emplace(open.channel, open.value_size);
        }
        woken = std::move(known.watchers);
    }
    for (Watcher& watcher : woken) {
        const dstd::SharedPtr<PendingOp> call = watcher.call.lock();
        if (call && !call->is_done()) watcher.wake(why);
    }
}

dstd::Optional<Error> check_declarations(ChannelState& channel,
                                         dstd::Vector<PeerWait>& waiting) {
    for (std::uint32_t peer = 0; peer < channel.size(); ++peer) {
        if (peer == channel.rank()) continue;
        ChannelOpen wrong;
        std::uint64_t seen = 0;
        switch (channel.hub()->open(peer, channel.id(), wrong, seen)) {
            case ChannelHub::Open::Matched:
                break;
            case ChannelHub::Open::Mismatch:
                return mismatch_error(channel, peer, wrong);
            case ChannelHub::Open::Waiting:
                waiting.push_back(PeerWait{peer, seen});
                break;
        }
    }
    return dstd::nullopt;
}

namespace {

void watch_declaration(const dstd::WeakPtr<ChannelState>& weak_channel,
                       const dstd::WeakPtr<PendingOp>& weak_call,
                       std::uint32_t peer, std::uint64_t seen);

void declaration_woke(const dstd::WeakPtr<ChannelState>& weak_channel,
                      const dstd::WeakPtr<PendingOp>& weak_call,
                      std::uint32_t peer, const dstd::Error* why) {
    const dstd::SharedPtr<ChannelState> channel = weak_channel.lock();
    const dstd::SharedPtr<PendingOp> call = weak_call.lock();
    if (!channel || !call || call->is_done() || why != nullptr) return;
    ChannelOpen wrong;
    std::uint64_t seen = 0;
    const ChannelHub::Open open =
        channel->hub()->open(peer, channel->id(), wrong, seen);
    if (open == ChannelHub::Open::Mismatch)
        call->cancel_alone(mismatch_error(*channel, peer, wrong));
    else if (open == ChannelHub::Open::Waiting)
        watch_declaration(weak_channel, weak_call, peer, seen);
}

void watch_declaration(const dstd::WeakPtr<ChannelState>& weak_channel,
                       const dstd::WeakPtr<PendingOp>& weak_call,
                       std::uint32_t peer, std::uint64_t seen) {
    for (;;) {
        const dstd::SharedPtr<ChannelState> channel = weak_channel.lock();
        const dstd::SharedPtr<PendingOp> call = weak_call.lock();
        if (!channel || !call || call->is_done()) return;
        const bool is_waiting = channel->hub()->watch(
            peer, 0, weak_call,
            [weak_channel, weak_call, peer](const Error* why) {
                declaration_woke(weak_channel, weak_call, peer, why);
            },
            seen);
        if (is_waiting) return;
        // A declaration came after it was read: read again.
        ChannelOpen wrong;
        const ChannelHub::Open open =
            channel->hub()->open(peer, channel->id(), wrong, seen);
        if (open == ChannelHub::Open::Mismatch)
            call->cancel_alone(mismatch_error(*channel, peer, wrong));
        if (open != ChannelHub::Open::Waiting) return;
    }
}

}  // namespace

void watch_declarations(const dstd::SharedPtr<ChannelState>& channel,
                        const dstd::SharedPtr<PendingOp>& call,
                        const dstd::Vector<PeerWait>& waiting) {
    for (const PeerWait& wait : waiting)
        watch_declaration(channel, call, wait.peer, wait.seen);
}

ChannelState::ChannelState(dstd::SharedPtr<Backend> backend,
                           dstd::SharedPtr<backend::Messaging> messaging,
                           dstd::SharedPtr<ChannelHub> hub,
                           dstd::WeakPtr<GroupState> group,
                           std::uint64_t group_id, std::uint64_t id,
                           std::uint32_t size, std::uint32_t rank,
                           dstd::StringView name, std::uint32_t value_size,
                           ChannelOpts opts)
    : backend_(std::move(backend)),
      messaging_(std::move(messaging)),
      hub_(std::move(hub)),
      group_(std::move(group)),
      group_id_(group_id),
      id_(id),
      size_(size),
      rank_(rank),
      name_(name),
      value_size_(value_size),
      opts_(opts),
      declared_(size, 0) {}

backend::MessageKey ChannelState::send_key(std::uint32_t peer) const noexcept {
    return backend::MessageKey{
        group_id_, rank_, peer,
        backend::message_tag(backend::TagPurpose::ChannelMessage,
                             channel_index(id_))};
}

backend::MessageKey ChannelState::recv_key(
    std::uint32_t source) const noexcept {
    return backend::MessageKey{
        group_id_, source, rank_,
        backend::message_tag(backend::TagPurpose::ChannelMessage,
                             channel_index(id_))};
}

backend::MessageKey ChannelState::declare_key(
    std::uint32_t peer) const noexcept {
    return backend::MessageKey{
        group_id_, rank_, peer,
        backend::message_tag(backend::TagPurpose::ChannelOpen, 0)};
}

backend::StreamOpts ChannelState::stream_opts() const noexcept {
    return backend::StreamOpts{opts_.depth < 1 ? std::size_t{1} : opts_.depth,
                               opts_.overflow};
}

RankTurn ChannelState::next_rank() noexcept {
    if (size_ < 2) return RankTurn{rank_, 0};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!returned_.empty()) {
        // The oldest turn given back goes first.
        std::size_t oldest = 0;
        for (std::size_t at = 1; at < returned_.size(); ++at)
            if (returned_[at].stamp < returned_[oldest].stamp) oldest = at;
        const RankTurn turn = returned_[oldest];
        returned_.erase(returned_.begin() +
                        static_cast<std::ptrdiff_t>(oldest));
        return turn;
    }
    for (std::uint32_t tried = 0; tried < size_; ++tried) {
        const std::uint32_t peer = cursor_;
        cursor_ = (cursor_ + 1) % size_;
        if (peer != rank_) return RankTurn{peer, ++stamp_};
    }
    return RankTurn{rank_, 0};
}

void ChannelState::return_rank(const RankTurn& turn) noexcept {
    if (size_ < 2) return;
    std::lock_guard<std::mutex> lock(mutex_);
    returned_.push_back(turn);
}

void ChannelState::declare(std::uint32_t peer) {
    if (peer >= size_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (declared_[peer] != 0) return;
        declared_[peer] = 1;
    }
    ChannelOpen open{id_, value_size_, 0};
    dstd::Vector<std::byte> bytes(sizeof(open));
    std::memcpy(bytes.data(), &open, sizeof(open));
    dstd::SharedPtr<ChannelState> self = this->shared_from_this();
    messaging_->send(declare_key(peer), std::move(bytes),
                     backend::HOLD_EVERYTHING,
                     [self, peer](const dstd::Error* why) {
                         if (why == nullptr) return;
                         std::lock_guard<std::mutex> lock(self->mutex_);
                         self->declared_[peer] = 0;
                     });
}

bool ChannelState::tx_closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return close_sent_;
}

void ChannelState::close_sending() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (close_sent_) return;
        close_sent_ = true;
    }
    for (std::uint32_t peer = 0; peer < size_; ++peer) {
        if (peer == rank_) continue;
        // A receiver that no send declared this channel to still sees its
        // close.
        declare(peer);
        messaging_->close(send_key(peer));
    }
}

std::uint64_t ChannelState::default_timeout() const noexcept {
    const dstd::SharedPtr<GroupState> group = group_.lock();
    if (!group) return 0;
    std::lock_guard<std::mutex> lock(group->mutex);
    return rank_ < group->default_timeout_ns.size()
               ? group->default_timeout_ns[rank_]
               : 0;
}

dstd::SharedPtr<ChannelState> open_channel(
    const dstd::SharedPtr<Backend>& backend,
    const dstd::SharedPtr<GroupState>& group, std::uint32_t rank,
    std::uint64_t id, dstd::StringView name, std::uint32_t value_size,
    ChannelOpts opts) {
    if (!group || group->size() < 2) return {};
    dstd::SharedPtr<ChannelHub> hub;
    {
        std::lock_guard<std::mutex> lock(group->mutex);
        if (group->channel_hubs.size() <= rank)
            group->channel_hubs.resize(static_cast<std::size_t>(rank) + 1);
        if (!group->channel_hubs[rank])
            group->channel_hubs[rank] = dstd::make_shared<ChannelHub>(
                backend->messaging, group->id, rank, group->size());
        hub = group->channel_hubs[rank];
    }
    auto state = dstd::make_shared<ChannelState>(
        backend, backend->messaging, hub, dstd::WeakPtr<GroupState>(group),
        group->id, id, group->size(), rank, name, value_size, opts);
    hub->note_open(id, value_size);
    return state;
}

Error empty_channel_error() {
    return Error::from_format<"a channel needs a group of two ranks or more">(
        Condition::InvalidArgument);
}

Error no_peer_error(const ChannelState& channel, std::uint32_t peer) {
    return Error::from_format<"channel '{}': the group holds no rank {}">(
        Condition::InvalidArgument, channel.name(), peer);
}

Error closed_error(const ChannelState& channel, std::uint32_t peer) {
    if (peer == backend::ANY_SOURCE)
        return Error::from_format<"channel '{}': every other rank closed it">(
            ChannelError::Closed, channel.name());
    return Error::from_format<"channel '{}': rank {} closed it">(
        ChannelError::Closed, channel.name(), peer);
}

Error closed_send_error(const ChannelState& channel) {
    return Error::from_format<"channel '{}': rank {} closed its sending end">(
        ChannelError::Closed, channel.name(), channel.rank());
}

Error full_error(const ChannelState& channel, std::uint32_t peer,
                 std::size_t depth) {
    return Error::from_format<
        "channel '{}': rank {} sends to rank {}, whose stream holds its {} "
        "messages">(ChannelError::Full, channel.name(), channel.rank(), peer,
                    depth);
}

Error mismatch_error(const ChannelState& channel, std::uint32_t peer,
                     const ChannelOpen& opened) {
    return Error::from_format<
        "channel '{}': rank {} opened it {} bytes wide and rank {} opened it "
        "{} bytes wide">(ChannelError::WrongChannel, channel.name(),
                         channel.rank(), channel.value_size(), peer,
                         opened.value_size);
}

Error with_hint(const ChannelState& channel, std::uint32_t peer, Error why) {
    if (!why.is(Condition::Timeout)) return why;
    dstd::String ids;
    const char* separator = "";
    for (std::uint32_t from = 0; from < channel.size(); ++from) {
        if (from == channel.rank()) continue;
        if (peer != backend::ANY_SOURCE && peer != from) continue;
        for (const std::uint64_t id : channel.hub()->unopened(from)) {
            dstd::fmt_append<"{}rank {} channel {:x}">(ids, separator, from,
                                                       id);
            separator = ", ";
        }
    }
    if (ids.empty()) return why;
    return Error::from_format<
        "{}; declared channels this rank did not open: {}">(Condition::Timeout,
                                                            why.message(), ids);
}

Error size_error(const ChannelState& channel, std::uint32_t peer,
                 std::size_t bytes) {
    return Error::from_format<
        "channel '{}': a message of rank {} holds {} bytes, not the {} bytes "
        "of one value">(Condition::InvalidArgument, channel.name(), peer, bytes,
                        channel.value_size());
}

Error gave_up(const ChannelState& channel, std::uint32_t peer,
              const dstd::Error& why) {
    if (why.is_code(ChannelError::Closed)) return closed_error(channel, peer);
    if (why.is_code(ChannelError::Full))
        return full_error(channel, peer, channel.stream_opts().depth);
    if (why.is_code(ChannelError::TooBig))
        return Error::from_format<"channel '{}': rank {}: {}">(
            ChannelError::TooBig, channel.name(), peer, why.message());
    return why;
}

namespace {

/**
 * @brief One send of a channel: declares the open of the channel to the peer
 * once, then sends the bytes of the value.
 *
 * The declaration comes first, so a receive checks the open of its peer before
 * it looks for a message that a mismatch would never send.
 */
class SendCall final : public Call<void>,
                       public std::enable_shared_from_this<SendCall> {
   public:
    SendCall(dstd::SharedPtr<ChannelState> channel, std::uint32_t peer) noexcept
        : Call<void>(std::move(channel)), peer_(peer) {}

    void start(dstd::Vector<std::byte> bytes) {
        const dstd::SharedPtr<ChannelState>& channel = Call<void>::channel();
        if (fail_if_cancelled()) return;
        if (peer_ >= channel->size()) {
            this->fail(no_peer_error(*channel, peer_));
            return;
        }
        if (channel->tx_closed()) {
            this->fail(closed_send_error(*channel));
            return;
        }
        if (channel->value_size() != 0 &&
            bytes.size() != channel->value_size()) {
            this->fail(size_error(*channel, peer_, bytes.size()));
            return;
        }
        channel->declare(peer_);

        auto self = this->shared_from_this();
        const backend::MessageKey key = channel->send_key(peer_);
        const backend::OpId op = channel->messaging()->send(
            key, std::move(bytes), channel->stream_opts(),
            [self](const dstd::Error* why) { self->sent(why); });
        this->note(key, op);
    }

   private:
    void sent(const dstd::Error* why) {
        if (why == nullptr) {
            this->succeed();
            return;
        }
        this->fail(gave_up(*Call<void>::channel(), peer_, *why));
    }

    std::uint32_t peer_ = 0;
};

/// One receive of a channel of bytes into the buffer of its caller.
class TakeBytes final : public Call<std::size_t>,
                        public std::enable_shared_from_this<TakeBytes> {
   public:
    TakeBytes(dstd::SharedPtr<ChannelState> channel,
              dstd::Span<std::byte> buffer) noexcept
        : Call<std::size_t>(std::move(channel)), buffer_(buffer) {}

    void start() {
        const dstd::SharedPtr<ChannelState>& channel =
            Call<std::size_t>::channel();
        if (fail_if_cancelled()) return;
        dstd::Vector<PeerWait> waiting;
        dstd::Optional<Error> mismatch = check_declarations(*channel, waiting);
        if (mismatch) {
            this->fail(std::move(*mismatch));
            return;
        }
        auto self = this->shared_from_this();
        const backend::MessageKey key = channel->recv_key(backend::ANY_SOURCE);
        const backend::OpId op = channel->messaging()->recv_into(
            key, channel->size() - 1, buffer_,
            [self](std::uint32_t source, std::size_t taken,
                   const dstd::Error* why) { self->took(source, taken, why); });
        this->note(key, op);
        if (!this->is_done()) watch_declarations(channel, self, waiting);
    }

   private:
    void took(std::uint32_t source, std::size_t taken, const dstd::Error* why) {
        if (why == nullptr) {
            this->succeed(taken);
            return;
        }
        const std::uint32_t peer =
            why->is_code(ChannelError::TooBig) ? source : backend::ANY_SOURCE;
        const dstd::SharedPtr<ChannelState>& channel =
            Call<std::size_t>::channel();
        this->fail(with_hint(*channel, backend::ANY_SOURCE,
                             gave_up(*channel, peer, *why)));
    }

    dstd::Span<std::byte> buffer_;
};

}  // namespace

Op<void> send_bytes(const dstd::SharedPtr<ChannelState>& channel,
                    std::uint32_t peer, dstd::Vector<std::byte> bytes,
                    const CallOpts& opts) {
    auto call = dstd::make_shared<SendCall>(channel, peer);
    stop_call(call, opts);
    call->start(std::move(bytes));
    watch_deadline(channel, call, opts);
    return Op<void>(std::move(call), dstd::SharedPtr<GroupState>{},
                    channel->rank());
}

Op<std::size_t> take_bytes(const dstd::SharedPtr<ChannelState>& channel,
                           dstd::Span<std::byte> buffer, const CallOpts& opts) {
    auto call = dstd::make_shared<TakeBytes>(channel, buffer);
    stop_call(call, opts);
    call->start();
    watch_deadline(channel, call, opts);
    return Op<std::size_t>(std::move(call), dstd::SharedPtr<GroupState>{},
                           channel->rank());
}

void watch_deadline(const dstd::SharedPtr<ChannelState>& channel,
                    const dstd::SharedPtr<PendingOp>& call,
                    const CallOpts& opts) {
    if (!channel || !call || call->is_done()) return;
    const dstd::SharedPtr<Backend>& backend = channel->backend();
    if (!backend) return;
    coro::TimerService* timers = backend->bootstrap->timers();
    if (timers == nullptr) return;
    std::uint64_t deadline = opts.deadline_ns;
    if (deadline == 0) {
        const std::uint64_t timeout = channel->default_timeout();
        if (timeout != 0) deadline = dstd::steady_now_ns() + timeout;
    }
    if (deadline == 0) return;
    const std::uint64_t now = dstd::steady_now_ns();
    const std::chrono::nanoseconds delay{deadline > now ? deadline - now : 0};
    auto target = dstd::make_shared<TimerTarget>();
    target->op = call;
    const coro::TimerService::TimerId timer =
        timers->register_timeout(delay, [target, deadline]() {
            dstd::SharedPtr<PendingOp> op;
            if (!target->take_call(op)) return;
            if (op->is_done()) return;
            op->cancel_alone(Error::from_format<"the deadline {} passed">(
                Condition::Timeout, deadline));
        });
    call->set_timeout(timers, timer, std::move(target));
}

}  // namespace detail
}  // namespace dftracer::utils::comm
