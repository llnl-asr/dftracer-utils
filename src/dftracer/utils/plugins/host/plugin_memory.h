// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_MEMORY_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_MEMORY_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/pool.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::plugins::host {

class PluginMemory;

/// One dftu_reservation: the bytes a plugin claimed in its pool, the most it
/// may hold, and the function that gives bytes back.
struct PluginReservation {
    PluginMemory* owner = nullptr;
    memory::Reservation held;
    dstd::String label;
    std::uint64_t cap = 0;
    const void* tag = nullptr;
    std::uint64_t (*reclaimer)(void* ud, std::uint64_t want) = nullptr;
    void* reclaimer_ud = nullptr;
    bool reclaiming = false;
    /// Set when the plugin released it, so a loop that still holds it skips it.
    std::atomic<bool> dead{false};
};

/// The pool of one loaded plugin and the blocks and reservations the plugin
/// made in it, shared by every fold of that plugin. The pool is named after
/// the plugin, so its bytes show under that name in the pool gauges. Closing
/// it releases the reservations the plugin left.
class PluginMemory {
   public:
    explicit PluginMemory(dstd::StringView plugin_name);
    ~PluginMemory();

    PluginMemory(const PluginMemory&) = delete;
    PluginMemory& operator=(const PluginMemory&) = delete;

    /// Null when the heap cannot make the block or the alignment is not a
    /// power of two up to 4096.
    void* alloc(std::uint64_t size, std::uint32_t align) noexcept;
    void* realloc(void* p, std::uint64_t old_size, std::uint64_t new_size,
                  std::uint32_t align) noexcept;
    void free(void* p, std::uint64_t size) noexcept;

    /// A new empty reservation. All reservations of the plugin together hold
    /// at most `cap` bytes, no limit when `cap` is 0. `tag` names the fold
    /// that polls it, see reclaim. Null after close().
    PluginReservation* reserve(dstd::StringView label, std::uint64_t cap,
                               const void* tag = nullptr) noexcept;
    /// Hands the reservations made with `from` to `to`.
    void retag(const void* from, const void* to) noexcept;
    /// Bytes held by the reservations made with `tag`.
    std::uint64_t held_by(const void* tag) noexcept;
    /// Asks the reclaimers of the reservations made with `tag` for `want`
    /// bytes in all, takes what they report off the reservations and returns
    /// it. Runs on the thread that owns those reservations.
    std::uint64_t reclaim(const void* tag, std::uint64_t want) noexcept;
    /// Adds `bytes`, running the reclaimer once when the first try is
    /// refused. On a refusal `why` says what stopped it.
    bool try_grow(PluginReservation& r, std::uint64_t bytes,
                  dstd::String& why) noexcept;
    void shrink(PluginReservation& r, std::uint64_t bytes) noexcept;
    void release(PluginReservation* r) noexcept;

    /// Releases the reservations made with `tag` that the plugin did not, logs
    /// each, and returns how many it released. For a cursor that is closed.
    std::size_t release_tagged(const void* tag) noexcept;

    /// Releases every reservation the plugin did not, logs each, and closes the
    /// pool. Returns how many it released. A second call returns 0.
    std::size_t close() noexcept;

    memory::Pool& pool() const noexcept { return *pool_; }
    const dstd::String& name() const noexcept { return name_; }

   private:
    dstd::String name_;
    memory::PoolHandle pool_handle_;
    memory::Pool* pool_ = nullptr;
    std::atomic<std::uint64_t> held_total_{0};
    std::mutex mutex_;
    dstd::Vector<dstd::SharedPtr<PluginReservation>> reservations_;
    bool closed_ = false;
};

/// Listens for the memory manager's requests to give bytes back to the pool of
/// one plugin, on behalf of the fold or cursor that owns the reservations made
/// with a tag. It does not move: the manager holds the address of its holder.
class MemoryWatch {
   public:
    MemoryWatch() = default;
    MemoryWatch(const MemoryWatch&) = delete;
    MemoryWatch& operator=(const MemoryWatch&) = delete;

    /// Starts listening; called when the first reservation is made.
    void watch(PluginMemory& memory);
    /// At a batch boundary: runs the reclaimers of the reservations made with
    /// `tag` for the bytes another holder asked of the pool. A no-op until
    /// watch().
    void poll(PluginMemory& memory, const void* tag);

   private:
    struct Holder final : memory::detail::Reclaimer {
        std::size_t reclaimable() const override { return 0; }
        std::size_t reclaim(std::size_t) override { return 0; }
    };
    Holder holder_;
    memory::detail::ReclaimSlot slot_;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_MEMORY_H
