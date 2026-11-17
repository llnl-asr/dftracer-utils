// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_PORTS_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_PORTS_H

#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/fileio.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace dftracer::utils::plugins {

/// Why a port call refused, as the code of a @ref dstd::Error. Test one with
/// `error.is_code(PortError::TooBig)`.
enum class PortError : std::int32_t {
    Full,     ///< no room; nothing was stored
    TooBig,   ///< the message or the buffer is too small
    Closed,   ///< the channel is closed, and drained for a receive
    NotReady  ///< nothing was set or published yet
};

inline constexpr dstd::ErrorDomain PORT_ERROR_DOMAIN =
    dstd::make_error_domain("dftracer.utils.plugins.ports");

inline constexpr dstd::ErrorCodeInfo PORT_ERROR_INFOS[] = {
    {&PORT_ERROR_DOMAIN, static_cast<std::int32_t>(PortError::Full),
     dstd::Condition::OutOfMemory, "PORT_FULL"},
    {&PORT_ERROR_DOMAIN, static_cast<std::int32_t>(PortError::TooBig),
     dstd::Condition::InvalidArgument, "PORT_TOO_BIG"},
    {&PORT_ERROR_DOMAIN, static_cast<std::int32_t>(PortError::Closed),
     dstd::Condition::Cancelled, "PORT_CLOSED"},
    {&PORT_ERROR_DOMAIN, static_cast<std::int32_t>(PortError::NotReady),
     dstd::Condition::NotFound, "PORT_NOT_READY"},
};

constexpr const dstd::ErrorCodeInfo& error_info(PortError code) noexcept {
    return PORT_ERROR_INFOS[static_cast<std::size_t>(code)];
}

/// What a send does when the channel has no free credit; see
/// DFTU_OVERFLOW_BLOCK.
enum class Overflow : std::int32_t {
    Block = DFTU_OVERFLOW_BLOCK,
    Fail = DFTU_OVERFLOW_FAIL,
    DropOldest = DFTU_OVERFLOW_DROP_OLDEST
};

/// The named merge of a phase result; see dftu_merge_op.
enum class MergeOp : std::int32_t {
    Sum = DFTU_MERGE_SUM,
    Min = DFTU_MERGE_MIN,
    Max = DFTU_MERGE_MAX,
    Union = DFTU_MERGE_UNION,
    Concat = DFTU_MERGE_CONCAT
};

/// The value type of a phase result; see MergeOp for which merge takes which.
enum class PhaseType : std::int32_t {
    I64 = DFTU_T_I64,
    U64 = DFTU_T_U64,
    F64 = DFTU_T_F64,
    Bytes = DFTU_T_BYTES
};

/// What a channel declaration sets; a field left empty takes the host default.
/// `task_end` says the plugin touches the channel only from tasks it starts.
/// `sends` and `receives` say which way this plugin uses the channel; a Block
/// channel needs one of them at every end, for the host to order the plugins.
struct ChannelOptions {
    dstd::Optional<std::uint32_t> credits;
    dstd::Optional<Overflow> overflow;
    dstd::Optional<std::uint32_t> max_msg_bytes;
    bool task_end = false;
    bool sends = false;
    bool receives = false;
};

/// The host's handle of a channel.
struct ChannelId {
    std::uint64_t raw = 0;
};

/// A latest value with its generation, the host's count of sets from 1.
struct Latest {
    std::uint32_t size = 0;
    std::uint64_t generation = 0;
};

namespace detail {

/// The error of a port outcome other than DFTU_PORT_OK.
inline dstd::Error port_error(dftu_port_status status, dstd::StringView what,
                              std::uint32_t needed) {
    switch (status) {
        case DFTU_PORT_FULL:
            return dstd::Error(PortError::Full,
                               dstd::fmt<"{}: the channel is full">(what));
        case DFTU_PORT_TOO_BIG:
            return dstd::Error(
                PortError::TooBig,
                dstd::fmt<"{}: the message is over the size limit, or the "
                          "buffer is short; it needs {} bytes">(what, needed));
        case DFTU_PORT_CLOSED:
            return dstd::Error(PortError::Closed,
                               dstd::fmt<"{}: the channel is closed">(what));
        default:
            return dstd::Error(PortError::NotReady,
                               dstd::fmt<"{}: nothing is ready yet">(what));
    }
}

/// The outcome slots of one async port call. The host writes them before the
/// task completes, so they live on the heap until the awaiter reads them.
struct PortSlots {
    dftu_port_status status = DFTU_PORT_OK;
    std::uint32_t len = 0;
};

}  // namespace detail

/// Typed wrapper over the dftu.svc.ports service. Every call gives a
/// @ref dstd::Result, and a call that can wait gives an awaitable of one. A
/// host without the channel members refuses with DFTU_E_UNSUPPORTED. Payloads
/// are bytes, a fixed-size value or a frame, and the host copies a payload
/// before a call returns.
class Ports {
   public:
    Ports(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          e_(h && h->get_service ? static_cast<const dftu_svc_ports*>(
                                       h->get_service(h->h, DFTU_SVC_PORTS))
                                 : nullptr) {}

    /// Declare (in a plugin factory) or open (in a callback) a channel. A bare
    /// name is the plugin's own; another plugin's channel is named in full,
    /// `plugin.<owner>.<name>`.
    dstd::Result<ChannelId> open(dstd::CStringView name,
                                 ChannelOptions options = {}) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        dftu_channel_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.name = name.c_str();
        if (options.credits) {
            desc.field_mask |= DFTU_CHANNEL_F_CREDITS;
            desc.credits = *options.credits;
        }
        if (options.overflow) {
            desc.field_mask |= DFTU_CHANNEL_F_OVERFLOW;
            desc.overflow = static_cast<dftu_overflow>(*options.overflow);
        }
        if (options.max_msg_bytes) {
            desc.field_mask |= DFTU_CHANNEL_F_MAX_MSG_BYTES;
            desc.max_msg_bytes = *options.max_msg_bytes;
        }
        if (options.task_end) desc.field_mask |= DFTU_CHANNEL_F_TASK_END;
        if (options.sends) desc.field_mask |= DFTU_CHANNEL_F_SENDS;
        if (options.receives) desc.field_mask |= DFTU_CHANNEL_F_RECEIVES;
        ChannelId id;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e->channel_open(ctx_, h_->h, &desc, &id.raw)));
        return id;
    }

    /// A send. `co_await` gives success, or a @ref PortError for a full
    /// (fail mode), too big or closed channel. Under Overflow::Block it waits
    /// for a credit.
    auto send(ChannelId ch, dstd::ByteView data) const {
        auto slots = dstd::make_shared<detail::PortSlots>();
        dftu_task* t = nullptr;
        dftu_status st = DFTU_E_UNSUPPORTED;
        if (const dftu_svc_ports* e = ext(); e != nullptr) {
            st = e->channel_send(ctx_, h_->h, ch.raw, data.data(),
                                 static_cast<std::uint32_t>(data.size()),
                                 &slots->status, &t);
        }
        auto finish = [slots](dftu_status) -> dstd::Result<void> {
            if (slots->status == DFTU_PORT_OK) return {};
            return detail::port_error(slots->status, "send", 0);
        };
        return AsyncValue<dstd::Result<void>, decltype(finish)>(
            detail::started(h_, st, t), finish);
    }

   private:
    auto recv_with(ChannelId ch, dstd::MutableByteView buf,
                   std::uint32_t* needed, bool exact) const {
        auto slots = dstd::make_shared<detail::PortSlots>();
        dftu_task* t = nullptr;
        dftu_status st = DFTU_E_UNSUPPORTED;
        if (const dftu_svc_ports* e = ext(); e != nullptr) {
            const auto recv = exact ? e->channel_recv_exact : e->channel_recv;
            st = recv(ctx_, h_->h, ch.raw, buf.data(),
                      static_cast<std::uint32_t>(buf.size()), &slots->len,
                      &slots->status, &t);
        }
        auto finish = [slots,
                       needed](dftu_status) -> dstd::Result<std::uint32_t> {
            if (needed != nullptr) *needed = slots->len;
            if (slots->status == DFTU_PORT_OK) return slots->len;
            return detail::port_error(slots->status, "recv", slots->len);
        };
        return AsyncValue<dstd::Result<std::uint32_t>, decltype(finish)>(
            detail::started(h_, st, t), finish);
    }

   public:
    /// A receive into `buf`. `co_await` gives the message length, or a
    /// @ref PortError: TooBig when `buf` is short (the message stays queued,
    /// and `*needed` gets its size when set) and Closed once the channel is
    /// closed and drained. `buf` and `needed` must outlive the await.
    auto recv(ChannelId ch, dstd::MutableByteView buf,
              std::uint32_t* needed = nullptr) const {
        return recv_with(ch, buf, needed, false);
    }

    /// A receive of a message that is exactly `buf.size()` bytes. A message of
    /// another length is taken off the queue and dropped, and `co_await` gives
    /// PortError::TooBig with `*needed` set to the dropped length, so it never
    /// blocks the channel. `buf` and `needed` must outlive the await.
    auto recv_exact(ChannelId ch, dstd::MutableByteView buf,
                    std::uint32_t* needed = nullptr) const {
        return recv_with(ch, buf, needed, true);
    }

    /// A send that never waits; a full channel is PortError::Full whatever its
    /// overflow mode, except Overflow::DropOldest, which drops the oldest.
    dstd::Result<void> try_send(ChannelId ch, dstd::ByteView data) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        dftu_port_status status = DFTU_PORT_OK;
        DFTRACER_UTILS_TRY_VOID(detail::reported(e->channel_try_send(
            ctx_, h_->h, ch.raw, data.data(),
            static_cast<std::uint32_t>(data.size()), &status)));
        if (status == DFTU_PORT_OK) return {};
        return detail::port_error(status, "try_send", 0);
    }

    /// End the sends; a receive still gets what was queued, then Closed.
    dstd::Result<void> close(ChannelId ch) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        return detail::reported(e->channel_close(ctx_, h_->h, ch.raw));
    }

    /// Set the latest value under `name`; the generation is the host's.
    dstd::Result<std::uint64_t> latest_set(dstd::CStringView name,
                                           dstd::ByteView data) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        std::uint64_t generation = 0;
        DFTRACER_UTILS_TRY_VOID(detail::reported(e->latest_set(
            ctx_, h_->h, name.c_str(), data.data(),
            static_cast<std::uint32_t>(data.size()), &generation)));
        return generation;
    }

    /// Read the latest value into `buf` without waiting. NotReady before the
    /// first set; TooBig when `buf` is short.
    dstd::Result<Latest> latest_get(dstd::CStringView name,
                                    dstd::MutableByteView buf) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        Latest out;
        dftu_port_status status = DFTU_PORT_OK;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e->latest_get(ctx_, h_->h, name.c_str(), buf.data(),
                          static_cast<std::uint32_t>(buf.size()), &out.size,
                          &out.generation, &status)));
        if (status == DFTU_PORT_OK) return out;
        return detail::port_error(status, "latest_get", out.size);
    }

    /// Declare the next generation of the phase result `name` in a plugin
    /// factory; the host assigns it. A plan that skips a generation fails.
    dstd::Result<std::uint64_t> phase_declare(
        dstd::CStringView name, MergeOp op, PhaseType type,
        std::uint64_t generation = 0) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        std::uint64_t assigned = 0;
        DFTRACER_UTILS_TRY_VOID(detail::reported(e->phase_declare(
            ctx_, h_->h, name.c_str(), static_cast<dftu_merge_op>(op),
            static_cast<dftu_type>(type), generation, &assigned)));
        return assigned;
    }

    /// Merge `data` into generation `generation`; Closed once it is sealed.
    dstd::Result<void> phase_contribute(dstd::CStringView name,
                                        std::uint64_t generation, MergeOp op,
                                        PhaseType type,
                                        dstd::ByteView data) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        dftu_port_status status = DFTU_PORT_OK;
        DFTRACER_UTILS_TRY_VOID(detail::reported(e->phase_contribute(
            ctx_, h_->h, name.c_str(), generation,
            static_cast<dftu_merge_op>(op), static_cast<dftu_type>(type),
            data.data(), static_cast<std::uint32_t>(data.size()), &status)));
        if (status == DFTU_PORT_OK) return {};
        return detail::port_error(status, "phase_contribute", 0);
    }

    /// Close generation `generation` to contributions; generations seal in
    /// order.
    dstd::Result<void> phase_seal(dstd::CStringView name,
                                  std::uint64_t generation) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        return detail::reported(
            e->phase_seal(ctx_, h_->h, name.c_str(), generation));
    }

    /// The merged result of a sealed generation, as its byte count; NotReady
    /// while the generation is open.
    dstd::Result<std::uint32_t> phase_result(dstd::CStringView name,
                                             std::uint64_t generation,
                                             dstd::MutableByteView buf) const {
        DFTRACER_UTILS_TRY(const dftu_svc_ports* e, service());
        std::uint32_t size = 0;
        dftu_port_status status = DFTU_PORT_OK;
        DFTRACER_UTILS_TRY_VOID(detail::reported(e->phase_result(
            ctx_, h_->h, name.c_str(), generation, buf.data(),
            static_cast<std::uint32_t>(buf.size()), &size, &status)));
        if (status == DFTU_PORT_OK) return size;
        return detail::port_error(status, "phase_result", size);
    }

   private:
    const dftu_svc_ports* ext() const noexcept {
        const bool covered =
            e_ != nullptr && e_->struct_size >= sizeof(dftu_svc_ports);
        return covered ? e_ : nullptr;
    }
    dstd::Result<const dftu_svc_ports*> service() const {
        const dftu_svc_ports* e = ext();
        if (e == nullptr) {
            return make_error(dstd::Condition::Unsupported,
                              "the host lacks the dftu.svc.ports channel "
                              "members");
        }
        return e;
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_svc_ports* e_;
};

inline Ports Host::ports() const { return Ports{h_, ctx_}; }

/// A channel of fixed-size values of `T`: a send copies `sizeof(T)` bytes and a
/// receive gives one back. `T` has no pointers, since the receiver may run in
/// another plugin.
template <class T>
class Channel {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a channel carries a trivially copyable value");

   public:
    Channel(Ports ports, ChannelId id) : ports_(ports), id_(id) {}

    auto send(const T& value) const {
        return ports_.send(
            id_,
            dstd::ByteView{reinterpret_cast<const char*>(&value), sizeof(T)});
    }

    dstd::Result<void> try_send(const T& value) const {
        return ports_.try_send(
            id_,
            dstd::ByteView{reinterpret_cast<const char*>(&value), sizeof(T)});
    }

    /// `co_await` gives the next value, or PortError::Closed at the end. A
    /// message of another size is taken off the queue and dropped, and gives
    /// PortError::TooBig with both sizes in its message.
    auto recv() const {
        auto slot = dstd::make_shared<Slot>();
        auto inner = ports_.recv_exact(
            id_, dstd::MutableByteView{slot->bytes, sizeof(T)}, &slot->got);
        auto finish =
            [slot](dstd::Result<std::uint32_t> got) -> dstd::Result<T> {
            if (!got) {
                if (!got.error().is_code(PortError::TooBig)) {
                    return std::move(got).error();
                }
                return dstd::Error(
                    PortError::TooBig,
                    dstd::fmt<"recv: the message is {} bytes and was "
                              "dropped; this channel carries {} bytes">(
                        slot->got, sizeof(T)));
            }
            T value;
            std::memcpy(&value, slot->bytes, sizeof(T));
            return value;
        };
        return Mapped<decltype(inner), decltype(finish)>(std::move(inner),
                                                         std::move(finish));
    }

    ChannelId id() const noexcept { return id_; }

   private:
    struct Slot {
        alignas(T) char bytes[sizeof(T)];
        std::uint32_t got = 0;
    };

    /// The await of `Inner`, with its result turned into a `T`.
    template <class Inner, class Finish>
    class [[nodiscard]] Mapped {
       public:
        Mapped(Inner inner, Finish finish)
            : inner_(std::move(inner)), finish_(std::move(finish)) {}

        bool await_ready() const noexcept { return inner_.await_ready(); }
        void await_suspend(std::coroutine_handle<Task::promise_type> h) {
            inner_.await_suspend(h);
        }
        dstd::Result<T> await_resume() {
            return finish_(inner_.await_resume());
        }

       private:
        Inner inner_;
        Finish finish_;
    };

    Ports ports_;
    ChannelId id_;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_PORTS_H */
