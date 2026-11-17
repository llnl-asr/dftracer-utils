// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_CHANNEL_H
#define DFTRACER_UTILS_COMM_CHANNEL_H

#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/detail/channel_state.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dftracer::utils::comm {

/**
 * @brief One message that arrived on a channel, and the rank that sent it.
 *
 * @p T is the type the channel carries, so a channel of bytes gives
 * @ref dstd::Vector of @ref std::byte.
 */
template <class T>
class Message {
   public:
    Message(std::uint32_t from, T value)
        : from_(from), value_(std::move(value)) {}

    /// The group rank that sent it.
    std::uint32_t from() const noexcept { return from_; }

    /// The value it carries.
    const T& value() const noexcept { return value_; }

   private:
    std::uint32_t from_ = 0;
    T value_;
};

namespace detail {

/// A channel of bytes carries values of any length, so it fixes no value size.
template <class T>
inline constexpr bool is_bytes_channel =
    std::is_same_v<T, dstd::Vector<std::byte>>;

/// The bytes of one value of @p T, which a channel of bytes leaves free.
template <class T>
constexpr std::uint32_t channel_value_size() noexcept {
    if constexpr (is_bytes_channel<T>) {
        return 0;
    } else {
        static_assert(std::is_trivially_copyable_v<T>,
                      "a channel carries a trivially copyable value");
        static_assert(!std::is_pointer_v<T>,
                      "a channel moves values, not an address of one rank");
        return static_cast<std::uint32_t>(sizeof(T));
    }
}

/// The id of a channel named @p name: the FNV-1a hash of the name, which both
/// ends compute the same way.
constexpr std::uint64_t channel_id(dstd::StringView name) noexcept {
    return dstd::hash::fnv1a_hash(name);
}

/// Copies the bytes of one message into a value of @p T: one plain copy for a
/// fixed-size value, one owned buffer for a channel of bytes.
template <class T>
T value_of(dstd::Span<const std::byte> bytes) {
    if constexpr (is_bytes_channel<T>) {
        dstd::Vector<std::byte> value(bytes.size());
        std::memcpy(value.data(), bytes.data(), bytes.size());
        return value;
    } else {
        T value{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }
}

/// A call of a channel that no group opened, which ends at once.
template <class T>
Op<T> empty_channel() {
    auto state = dstd::make_shared<OpState<T>>();
    state->fail(empty_channel_error());
    return Op<T>(std::move(state), dstd::SharedPtr<GroupState>{}, 0);
}

/// Cancels @p call at once when the stop token of @p opts is already stopped,
/// as a call of a group does.
template <class T>
void stop_call(const dstd::SharedPtr<T>& call, const CallOpts& opts) {
    if (opts.stop.stop_requested())
        call->cancel_alone(
            dstd::Error{dstd::Condition::Cancelled,
                        "the call was cancelled before it started"});
}

/// Guards @p call with the deadline of @p opts, which the timer thread cancels;
/// a passed deadline ends the call alone, never the group of the others.
void watch_deadline(const dstd::SharedPtr<ChannelState>& channel,
                    const dstd::SharedPtr<PendingOp>& call,
                    const CallOpts& opts);

/// As above for a call of any provider: @p default_timeout is the timeout of
/// the group, used when @p opts names no deadline.
void watch_deadline(const dstd::SharedPtr<Backend>& backend,
                    std::uint64_t default_timeout,
                    const dstd::SharedPtr<PendingOp>& call,
                    const CallOpts& opts);

/// Reads what every peer declared of the channel. Gives the error of the first
/// peer that opened another channel or another value size; otherwise it lists
/// in
/// @p waiting the peers that declared nothing of this channel yet.
dstd::Optional<dstd::Error> check_declarations(ChannelState& channel,
                                               dstd::Vector<PeerWait>& waiting);

/// Ends @p call with the error of a mismatch when one of the @p waiting peers
/// declares another channel later, so a receive of any sender never waits for a
/// message that a mismatch would not send.
void watch_declarations(const dstd::SharedPtr<ChannelState>& channel,
                        const dstd::SharedPtr<PendingOp>& call,
                        const dstd::Vector<PeerWait>& waiting);

/**
 * @brief One channel call: the stream it waits on and the handle its provider
 * gave it, so a cancel ends this call alone and not the group of the others.
 *
 * A channel call stands alone: cancelling it, or passing its deadline, ends it
 * with @ref dstd::Condition::Cancelled or @ref dstd::Condition::Timeout and
 * leaves every other call and every collective of the group alone.
 *
 * The call ends only through its provider, which decides a cancel under its own
 * lock: a call the provider still holds ends with the error of the cancel, so a
 * cancelled receive keeps its message for a later one, and a call the provider
 * already paired keeps its real outcome. A cancel that comes before the call
 * parked, or between two of its stages, is kept and ends the call before it
 * takes anything.
 */
template <class T>
class Call : public OpState<T> {
   public:
    explicit Call(dstd::SharedPtr<ChannelState> channel) noexcept
        : channel_(std::move(channel)) {}

    /// The channel the call moves bytes of.
    const dstd::SharedPtr<ChannelState>& channel() const noexcept {
        return channel_;
    }

    /// Notes the call the provider parked for this one, so a cancel reaches it.
    /// A cancel that came first reaches it here.
    void note(const backend::MessageKey& key, backend::OpId op) noexcept {
        bool requested = false;
        dstd::Error why;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stage_ = Stage::Provider;
            key_ = key;
            op_ = op;
            requested = requested_;
            if (requested) why = why_;
        }
        if (requested && op.value != 0 && channel_)
            (void)channel_->messaging()->cancel(key, op, std::move(why));
    }

    void cancel_alone(const dstd::Error& why) noexcept override {
        if (this->is_done()) return;
        Stage stage;
        backend::MessageKey key;
        backend::OpId op;
        std::uint32_t peer = 0;
        std::uint64_t ticket = 0;
        dstd::Error reason;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!requested_) {
                requested_ = true;
                why_ = why;
            }
            reason = why_;
            stage = stage_;
            key = key_;
            op = op_;
            peer = peer_;
            ticket = ticket_;
        }
        if (!channel_) return;
        if (stage == Stage::Provider) {
            if (op.value != 0)
                (void)channel_->messaging()->cancel(key, op, std::move(reason));
        } else if (stage == Stage::Watch) {
            if (channel_->hub()->unwatch(peer, ticket))
                end_with(std::move(reason));
        }
    }

   protected:
    /// Ends the call with @p why.
    virtual void end_with(dstd::Error why) { this->fail(std::move(why)); }

    /// Ends the call when a cancel reached it before the stage that follows,
    /// and gives whether it did.
    bool fail_if_cancelled() {
        dstd::Error why;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!requested_) return false;
            why = why_;
        }
        end_with(std::move(why));
        return true;
    }

    /// Waits for the next declaration of @p peer, which wakes @p wake, and
    /// gives whether it waits: false when @p peer declared since @p seen was
    /// read.
    /// @p self is this call.
    bool watch_peer(std::uint32_t peer, ChannelHub::Wake wake,
                    const dstd::WeakPtr<PendingOp>& self, std::uint64_t seen) {
        std::uint64_t ticket = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stage_ = Stage::Watch;
            peer_ = peer;
            ticket = channel_->hub()->make_ticket();
            ticket_ = ticket;
        }
        if (!channel_->hub()->watch(peer, ticket, self, std::move(wake), seen))
            return false;
        dstd::Error why;
        bool requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested =
                requested_ && stage_ == Stage::Watch && ticket_ == ticket;
            if (requested) why = why_;
        }
        if (requested && channel_->hub()->unwatch(peer, ticket))
            end_with(std::move(why));
        return true;
    }

   private:
    /// What the call waits on now.
    enum class Stage : std::uint8_t { None, Provider, Watch };

    dstd::SharedPtr<ChannelState> channel_;
    std::mutex mutex_;
    Stage stage_ = Stage::None;
    backend::MessageKey key_;
    backend::OpId op_;
    std::uint32_t peer_ = 0;
    std::uint64_t ticket_ = 0;
    bool requested_ = false;
    dstd::Error why_;
};

/**
 * @brief One receive of a value, from one peer or from any sender.
 *
 * The receive checks the open of its peer, so an end that opened the channel
 * with another value type fails with an error that names the channel and both
 * ranks instead of waiting for a message that never comes. A peer that opened
 * another name cannot be told from one that opens this channel later, so the
 * receive waits, and its deadline error names what the peer declared.
 */
template <class T>
class TakeCall final : public Call<Message<T>>,
                       public std::enable_shared_from_this<TakeCall<T>> {
   public:
    TakeCall(dstd::SharedPtr<ChannelState> channel, std::uint32_t peer,
             std::uint64_t turn) noexcept
        : Call<Message<T>>(std::move(channel)), peer_(peer), turn_(turn) {}

    /// Starts the call.
    void start() {
        if (this->fail_if_cancelled()) return;
        if (peer_ == backend::ANY_SOURCE) {
            start_any();
            return;
        }
        if (peer_ >= Call<Message<T>>::channel()->size()) {
            end_with(no_peer_error(*Call<Message<T>>::channel(), peer_));
            return;
        }
        advance();
    }

   protected:
    void end_with(dstd::Error why) override {
        // A call that never took its message gives its turn back.
        if (turn_ != 0 && (why.is(dstd::Condition::Cancelled) ||
                           why.is(dstd::Condition::Timeout)))
            Call<Message<T>>::channel()->return_rank(RankTurn{peer_, turn_});
        this->fail(
            with_hint(*Call<Message<T>>::channel(), peer_, std::move(why)));
    }

   private:
    void start_any() {
        const dstd::SharedPtr<ChannelState>& channel =
            Call<Message<T>>::channel();
        dstd::Vector<PeerWait> waiting;
        dstd::Optional<dstd::Error> mismatch =
            check_declarations(*channel, waiting);
        if (mismatch) {
            end_with(std::move(*mismatch));
            return;
        }
        take();
        if (!this->is_done())
            watch_declarations(channel, this->shared_from_this(), waiting);
    }

    void advance() {
        const dstd::SharedPtr<ChannelState>& channel =
            Call<Message<T>>::channel();
        dstd::WeakPtr<TakeCall<T>> weak = this->weak_from_this();
        for (;;) {
            if (this->fail_if_cancelled()) return;
            ChannelOpen wrong;
            std::uint64_t seen = 0;
            const ChannelHub::Open open =
                channel->hub()->open(peer_, channel->id(), wrong, seen);
            if (open == ChannelHub::Open::Matched) {
                take();
                return;
            }
            if (open == ChannelHub::Open::Mismatch) {
                end_with(mismatch_error(*channel, peer_, wrong));
                return;
            }
            // The peer has not declared this channel yet: wait for its next
            // declaration, then look again. A declaration that came since it
            // was read is read again at once.
            const bool is_waiting = this->watch_peer(
                peer_,
                [weak](const dstd::Error* why) {
                    if (const dstd::SharedPtr<TakeCall<T>> self = weak.lock())
                        self->woke(why);
                },
                weak, seen);
            if (is_waiting) return;
        }
    }

    void woke(const dstd::Error* why) {
        if (why != nullptr) {
            end_with(*why);
            return;
        }
        advance();
    }

    void take() {
        if (this->fail_if_cancelled()) return;
        const dstd::SharedPtr<ChannelState>& channel =
            Call<Message<T>>::channel();
        auto self = this->shared_from_this();
        backend::MessageKey key;
        backend::OpId op;
        if (peer_ == backend::ANY_SOURCE) {
            key = channel->recv_key(backend::ANY_SOURCE);
            op = channel->messaging()->recv_any_source(
                key, channel->size() - 1,
                [self](std::uint32_t source, dstd::Span<const std::byte> bytes,
                       const dstd::Error* why) {
                    self->arrived(source, bytes, why);
                });
        } else {
            key = channel->recv_key(peer_);
            op = channel->messaging()->recv(
                key, [self](dstd::Span<const std::byte> bytes,
                            const dstd::Error* why) {
                    self->arrived(self->peer_, bytes, why);
                });
        }
        this->note(key, op);
    }

    void arrived(std::uint32_t source, dstd::Span<const std::byte> bytes,
                 const dstd::Error* why) {
        const dstd::SharedPtr<ChannelState>& channel =
            Call<Message<T>>::channel();
        if (why != nullptr) {
            end_with(gave_up(*channel, peer_, *why));
            return;
        }
        if (channel->value_size() != 0 &&
            bytes.size() != channel->value_size()) {
            end_with(size_error(*channel, source, bytes.size()));
            return;
        }
        this->set_received(bytes.size());
        this->succeed(Message<T>(source, value_of<T>(bytes)));
    }

    std::uint32_t peer_ = 0;
    std::uint64_t turn_ = 0;
};

/// Sends @p bytes to @p peer, and gives the call that ends when the provider
/// holds them.
Op<void> send_bytes(const dstd::SharedPtr<ChannelState>& channel,
                    std::uint32_t peer, dstd::Vector<std::byte> bytes,
                    const CallOpts& opts);

/// Receives one message of a channel of bytes into @p buffer.
Op<std::size_t> take_bytes(const dstd::SharedPtr<ChannelState>& channel,
                           dstd::Span<std::byte> buffer, const CallOpts& opts);

/// Sends the bytes of one value of the fixed-size @p T.
template <class T>
Op<void> send_value(const dstd::SharedPtr<ChannelState>& channel,
                    std::uint32_t peer, const T& value, const CallOpts& opts) {
    static_assert(!is_bytes_channel<T>,
                  "a channel of bytes sends a span of bytes");
    dstd::Vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return send_bytes(channel, peer, std::move(bytes), opts);
}

/// Receives one value of the fixed-size @p T from @p peer, or from any sender
/// for @ref backend::ANY_SOURCE.
template <class T>
Op<Message<T>> take_value(const dstd::SharedPtr<ChannelState>& channel,
                          std::uint32_t peer, const CallOpts& opts,
                          std::uint64_t turn = 0) {
    auto call = dstd::make_shared<TakeCall<T>>(channel, peer, turn);
    channel->observe(*call, CallKind::ChannelRecv);
    stop_call(call, opts);
    call->start();
    watch_deadline(channel, call, opts);
    return Op<Message<T>>(std::move(call), dstd::SharedPtr<GroupState>{},
                          channel->rank());
}

}  // namespace detail

template <class T>
class Channel;
template <class T>
class ChannelTx;
template <class T>
class ChannelRx;

/// The two ends of a channel, which move in opposite directions and can be
/// handed to different tasks.
template <class T>
struct ChannelHalves {
    ChannelTx<T> tx;
    ChannelRx<T> rx;
};

/**
 * @brief The sending end of a channel.
 *
 * A handle: copies name the same channel, and it is copyable so its owner can
 * hand it on. @ref close ends the sending end, so every message sent before it
 * arrives and the receivers then see @ref ChannelError::Closed, while a send
 * after it fails.
 */
template <class T>
class ChannelTx {
   public:
    /// The end of a channel that no group opened, which fails every send.
    ChannelTx() noexcept = default;

    /// The id the name of the channel maps to.
    std::uint64_t id() const noexcept { return state_ ? state_->id() : 0; }

    /// Sends @p value to @p peer of the group.
    Op<void> send(std::uint32_t peer, const T& value, CallOpts opts = {}) {
        if (!state_) return detail::empty_channel<void>();
        return detail::send_value(state_, peer, value, opts);
    }

    /// Sends @p bytes to @p peer. A channel of bytes sends a span of any
    /// length; a channel of a fixed-size value sends exactly one value.
    Op<void> send(std::uint32_t peer, dstd::Span<const std::byte> bytes,
                  CallOpts opts = {}) {
        if (!state_) return detail::empty_channel<void>();
        return detail::send_bytes(
            state_, peer, dstd::Vector<std::byte>(bytes.begin(), bytes.end()),
            opts);
    }

    /// Sends the close of this channel to every peer.
    void close();

   private:
    friend class Channel<T>;
    friend struct ChannelHalves<T>;
    explicit ChannelTx(dstd::SharedPtr<detail::ChannelState> state) noexcept
        : state_(std::move(state)) {}
    dstd::SharedPtr<detail::ChannelState> state_;
};

/**
 * @brief The receiving end of a channel.
 *
 * A handle: copies name the same channel. A receive takes the first message its
 * sender sent that no earlier receive took, so the messages of one sender
 * arrive in the order it sent them.
 */
template <class T>
class ChannelRx {
   public:
    /// The end of a channel that no group opened, which fails every receive.
    ChannelRx() noexcept = default;

    /// The id the name of the channel maps to.
    std::uint64_t id() const noexcept { return state_ ? state_->id() : 0; }

    /// Receives one message from any sender.
    Op<Message<T>> recv(CallOpts opts = {}) {
        if (!state_) return detail::empty_channel<Message<T>>();
        return detail::take_value<T>(state_, backend::ANY_SOURCE, opts);
    }

    /// Receives one message from @p peer.
    Op<Message<T>> recv_from(std::uint32_t peer, CallOpts opts = {}) {
        if (!state_) return detail::empty_channel<Message<T>>();
        return detail::take_value<T>(state_, peer, opts);
    }

    /// Receives one message from each other rank in rank order: each call takes
    /// the message of the next rank, and the ranks repeat once each had one.
    /// A call that is cancelled or passes its deadline gives its turn back, so
    /// the next call waits for the same rank.
    Op<Message<T>> recv_in_rank_order(CallOpts opts = {}) {
        if (!state_) return detail::empty_channel<Message<T>>();
        const detail::RankTurn turn = state_->next_rank();
        return detail::take_value<T>(state_, turn.peer, opts, turn.stamp);
    }

    /// Receives one message of a channel of bytes into @p buffer, and gives how
    /// many bytes of it @p buffer took. A message longer than the buffer fails
    /// with @ref ChannelError::TooBig and stays first, so a later receive takes
    /// it again.
    Op<std::size_t> recv_into(dstd::Span<std::byte> buffer,
                              CallOpts opts = {}) {
        static_assert(detail::is_bytes_channel<T>,
                      "only a channel of bytes receives into a buffer");
        if (!state_) return detail::empty_channel<std::size_t>();
        return detail::take_bytes(state_, buffer, opts);
    }

   private:
    friend class Channel<T>;
    friend struct ChannelHalves<T>;
    explicit ChannelRx(dstd::SharedPtr<detail::ChannelState> state) noexcept
        : state_(std::move(state)) {}
    dstd::SharedPtr<detail::ChannelState> state_;
};

/**
 * @brief A channel of values of @p T between the ranks of a group, named once
 * and opened by every rank that uses it.
 *
 * A handle: copies name the same channel. The name maps to a 64-bit id by
 * FNV-1a, so two ranks that open one name share one channel, and the two ends
 * check that id and the bytes of one value at the first message: a peer that
 * opened another channel, or another value type, is an error that names the
 * channel and both ranks, never a wait.
 *
 * Opening a channel is collective-free. The messages of one sender to one
 * receiver on one channel arrive in the order their sender sent them, and a
 * channel is independent of every other channel and of the collectives of the
 * group: no channel message meets a collective message, and nothing orders one
 * against the other.
 *
 * A channel keeps a little state for each distinct channel name until its
 * group ends.
 *
 * A channel carries a trivially copyable @p T of a fixed size, or
 * @ref dstd::Vector of @ref std::byte for bytes of any length.
 */
template <class T>
class Channel {
   public:
    using value_type = T;

    /// The channel of no group, which fails every call.
    Channel() noexcept = default;

    /// The id the name of the channel maps to, which both ends share.
    std::uint64_t id() const noexcept { return state_ ? state_->id() : 0; }

    /// The name it was opened with, empty for a channel of no group.
    dstd::StringView name() const noexcept {
        return state_ ? dstd::StringView(state_->name()) : dstd::StringView{};
    }

    /// This rank's number in the group of the channel.
    std::uint32_t rank() const noexcept { return state_ ? state_->rank() : 0; }

    /// How many ranks the group of the channel has.
    std::uint32_t size() const noexcept { return state_ ? state_->size() : 0; }

    /// @ref ChannelTx::send.
    Op<void> send(std::uint32_t peer, const T& value, CallOpts opts = {}) {
        return tx().send(peer, value, opts);
    }

    /// @ref ChannelTx::send for a channel of bytes.
    Op<void> send(std::uint32_t peer, dstd::Span<const std::byte> bytes,
                  CallOpts opts = {}) {
        return tx().send(peer, bytes, opts);
    }

    /// @ref ChannelRx::recv.
    Op<Message<T>> recv(CallOpts opts = {}) { return rx().recv(opts); }

    /// @ref ChannelRx::recv_from.
    Op<Message<T>> recv_from(std::uint32_t peer, CallOpts opts = {}) {
        return rx().recv_from(peer, opts);
    }

    /// @ref ChannelRx::recv_in_rank_order.
    Op<Message<T>> recv_in_rank_order(CallOpts opts = {}) {
        return rx().recv_in_rank_order(opts);
    }

    /// @ref ChannelRx::recv_into.
    Op<std::size_t> recv_into(dstd::Span<std::byte> buffer,
                              CallOpts opts = {}) {
        return rx().recv_into(buffer, opts);
    }

    /// @ref ChannelTx::close.
    void close() { tx().close(); }

    /// The two ends, which move in opposite directions.
    ChannelHalves<T> split() const noexcept {
        return ChannelHalves<T>{tx(), rx()};
    }

    /// The sending end.
    ChannelTx<T> tx() const noexcept { return ChannelTx<T>(state_); }

    /// The receiving end.
    ChannelRx<T> rx() const noexcept { return ChannelRx<T>(state_); }

   private:
    friend class Group;
    explicit Channel(dstd::SharedPtr<detail::ChannelState> state) noexcept
        : state_(std::move(state)) {}
    dstd::SharedPtr<detail::ChannelState> state_;
};

template <class T>
void ChannelTx<T>::close() {
    if (state_) state_->close_sending();
}

template <class T>
Channel<T> Group::channel(dstd::StringView name, ChannelOpts opts) {
    return Channel<T>(
        detail::open_channel(backend_, state_, rank_, detail::channel_id(name),
                             name, detail::channel_value_size<T>(), opts));
}

template <class T>
Channel<T> Group::channel(dstd::StringView name) {
    return channel<T>(name, ChannelOpts{});
}

template <class Tag>
Channel<typename Tag::value_type> Group::channel(ChannelOpts opts) {
    return channel<typename Tag::value_type>(Tag::name, opts);
}

template <class Tag>
Channel<typename Tag::value_type> Group::channel() {
    return channel<typename Tag::value_type>(Tag::name, ChannelOpts{});
}

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_CHANNEL_H
