// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PORT_HUB_H
#define DFTRACER_UTILS_PLUGINS_HOST_PORT_HUB_H

#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/port_plan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::plugins::host {

/// One bounded channel between plugins of a scan. Messages are FIFO; the
/// channel owns the bytes of a queued message, since the receiver runs after
/// the sender returned. Safe from any thread.
class PortChannel {
   public:
    explicit PortChannel(const ChannelDecl& decl);
    PortChannel(const PortChannel&) = delete;
    PortChannel& operator=(const PortChannel&) = delete;

    /// Waits for a credit under DFTU_OVERFLOW_BLOCK, else never waits.
    coro::CoroTask<void> send(dstd::Vector<std::byte> message,
                              dftu_port_status* status);
    dftu_port_status try_send(dstd::Vector<std::byte> message);
    /// The send when it needs no wait: false, with `message` untouched, when a
    /// Block channel has no credit free.
    bool send_now(dstd::Vector<std::byte>& message, dftu_port_status* status);
    /// Waits for a message or the close; see dftu_svc_ports::channel_recv.
    /// With `exact`, a message whose length is not `cap` is consumed and
    /// dropped, and the status is DFTU_PORT_TOO_BIG with its length.
    coro::CoroTask<void> recv(void* buf, std::uint32_t cap,
                              std::uint32_t* out_len, dftu_port_status* status,
                              bool exact);
    /// The receive when a message or the close is already there; false when
    /// it would wait.
    bool recv_now(void* buf, std::uint32_t cap, std::uint32_t* out_len,
                  dftu_port_status* status, bool exact);
    void close();

    std::uint32_t max_msg_bytes() const { return decl_.max_msg_bytes; }
    std::int32_t overflow() const { return decl_.overflow; }
    const dstd::String& name() const { return decl_.name; }

   private:
    dftu_port_status push(dstd::Vector<std::byte> message, bool took_credit);
    void finish_recv(void* buf, std::uint32_t cap, std::uint32_t* out_len,
                     dftu_port_status* status, bool exact);

    ChannelDecl decl_;
    std::mutex mutex_;
    dstd::Deque<dstd::Vector<std::byte>> queue_;
    bool closed_ = false;
    coro::AsyncSemaphore space_;
    coro::AsyncSemaphore items_;
};

/// The latest value under one name. Its own mutex, so two names never wait on
/// each other.
class PortLatest {
   public:
    /// The generation of this set, counting from 1.
    std::uint64_t set(const void* data, std::uint32_t len);
    dftu_port_status get(void* buf, std::uint32_t cap, std::uint32_t* out_len,
                         std::uint64_t* out_generation);

   private:
    std::mutex mutex_;
    dstd::Vector<std::byte> bytes_;
    std::uint64_t generation_ = 0;
};

/// One phase result: its merge, its value type and its generations. Its own
/// mutex.
class PortPhase {
   public:
    PortPhase(dstd::String name, dftu_merge_op op, dftu_type type,
              std::uint64_t generations);

    dstd::Result<dftu_port_status> contribute(std::uint64_t generation,
                                              dftu_merge_op op, dftu_type type,
                                              const void* data,
                                              std::uint32_t len);
    dstd::Result<void> seal(std::uint64_t generation);
    /// Fills the u64 scalar of generation `generation` with `value` and seals
    /// it, as the host does before the scan for a seeded generation. From then
    /// on a plugin's contribute to it is an error.
    dstd::Result<void> seed(std::uint64_t generation, std::uint64_t value);
    dstd::Result<dftu_port_status> result(std::uint64_t generation, void* buf,
                                          std::uint32_t cap,
                                          std::uint32_t* out_len);

   private:
    struct Generation {
        bool sealed = false;
        bool seeded = false;
        bool any = false;
        std::uint64_t scalar = 0;
        dstd::Vector<std::uint64_t> set;
        dstd::Vector<std::byte> bytes;
    };

    dstd::Unexpected<dstd::Error> undeclared(std::uint64_t generation) const;

    dstd::String name_;
    dftu_merge_op op_;
    dftu_type type_;
    std::mutex mutex_;
    dstd::Vector<Generation> generations_;
};

/// The ports of one scan that cross plugins: the declared channels, the latest
/// values and the phase results. Built from the plan, so a name no plugin
/// declared never reaches it. The channel and phase tables never change after
/// the constructor, so a lookup takes no lock; the latest table grows under
/// one mutex that a lookup takes once, and the caller keeps the slot.
class PortHub {
   public:
    PortHub(const dstd::Vector<ChannelDecl>& channels,
            const dstd::Vector<PhaseDecl>& phases);
    ~PortHub();
    PortHub(const PortHub&) = delete;
    PortHub& operator=(const PortHub&) = delete;

    /// The handle of the declared channel `name`, which is the same for every
    /// end.
    dstd::Optional<std::uint64_t> find_channel(dstd::StringView name) const;
    PortChannel* channel(std::uint64_t handle) const;
    /// Closes every channel, which wakes every waiting send and receive.
    void close_all();

    /// The slot of `name`, made on first use; it lives as long as the hub.
    PortLatest* latest(dstd::StringView name);
    /// The phase result `name`, or null when no plan declared it.
    PortPhase* phase(dstd::StringView name) const;

   private:
    dstd::HashMap<std::uint64_t, dstd::UniquePtr<PortChannel>> channels_;
    dstd::HashMap<dstd::String, std::uint64_t> handles_;
    std::mutex latest_mutex_;
    dstd::HashMap<dstd::String, dstd::UniquePtr<PortLatest>> latest_;
    dstd::HashMap<dstd::String, dstd::UniquePtr<PortPhase>> phases_;
};

/// The names one plugin passed to the latest and phase calls, each qualified
/// and matched to its slot once. A call after the first with a name takes this
/// plugin's mutex for one lookup and the slot's own mutex, never the hub's.
class PortNames {
   public:
    struct Entry {
        dstd::String full;
        /// Null when no plan declared a phase result under `full`.
        PortPhase* phase = nullptr;
        std::atomic<PortLatest*> latest{nullptr};
    };

    PortNames(PortHub* hub, dstd::String space);

    /// The entry of `name`, which stays valid as long as this object.
    dstd::Result<Entry*> resolve(dstd::StringView name);
    /// The latest slot of `entry`, made on first use.
    PortLatest* latest(Entry* entry);

   private:
    PortHub* hub_;
    dstd::String space_;
    std::mutex mutex_;
    dstd::HashMap<dstd::String, dstd::UniquePtr<Entry>> entries_;
};

/// True on a thread that runs a plugin's on_batch, transform or state update.
/// A DFTU_OVERFLOW_BLOCK send started there would stop the loop that must
/// drain the channel, so the host refuses it.
bool in_batch_callback() noexcept;

/// Marks the thread as inside a batch callback while it lives.
class BatchCallbackScope {
   public:
    BatchCallbackScope() noexcept;
    ~BatchCallbackScope();
    BatchCallbackScope(const BatchCallbackScope&) = delete;
    BatchCallbackScope& operator=(const BatchCallbackScope&) = delete;

   private:
    bool saved_;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PORT_HUB_H
