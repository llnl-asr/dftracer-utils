// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_CURSOR_HOST_H
#define DFTRACER_UTILS_PLUGINS_HOST_CURSOR_HOST_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {

/// The host a provider's scan or a node's open hands its plugin, one per
/// cursor and valid until the cursor is destroyed. It serves the services of a
/// fold's host that need no scan, and the memory service charges the pool of
/// the plugin. The cursor's driver calls poll_reclaim between two next() calls,
/// which runs the reclaimers of the cursor's reservations when another holder
/// of the process budget asked the pool for bytes. Destroying it releases the
/// reservations the cursor left and logs each.
class CursorHost {
   public:
    /// `memory` is the pool of the plugin; null gives this cursor a pool named
    /// `fallback_name`. `memory_budget` is the budget a reservation is held to,
    /// 0 for the automatic one.
    CursorHost(dstd::SharedPtr<PluginMemory> memory,
               dstd::StringView fallback_name, std::uint64_t memory_budget);
    ~CursorHost();

    CursorHost(const CursorHost&) = delete;
    CursorHost& operator=(const CursorHost&) = delete;

    const ::dftu_plugin_host* host() const { return &host_; }

    PluginMemory& memory() { return *memory_; }
    std::uint64_t memory_budget() const { return memory_budget_; }
    /// Starts listening for the memory manager's requests to give bytes back;
    /// called when the plugin makes its first reservation.
    void watch_memory_requests() { watch_.watch(*memory_); }
    /// At a boundary between two next() calls: runs the reclaimers of this
    /// cursor's reservations for the bytes another holder asked of the pool. A
    /// no-op until a reservation exists.
    void poll_reclaim() { watch_.poll(*memory_, this); }

   private:
    dstd::SharedPtr<PluginMemory> memory_;
    std::uint64_t memory_budget_;
    MemoryWatch watch_;
    ::dftu_plugin_host host_{};
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_CURSOR_HOST_H
