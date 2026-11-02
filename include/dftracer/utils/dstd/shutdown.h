// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_SHUTDOWN_H
#define DFTRACER_UTILS_DSTD_SHUTDOWN_H

#include <cstdint>

namespace dftracer::utils::dstd {

/// The order the stages run in: CloseStores, StopRuntime, FreeCaches.
enum class ShutdownStage : std::uint8_t {
    CloseStores,
    StopRuntime,
    FreeCaches,
};

/// Runs `hook` in `stage` at shutdown.
///
/// Hooks of one stage run in reverse order of registration, so the hook added
/// last runs first. A hook must not make a runtime and must not keep memory
/// the memory exit check counts. Safe from any thread. A hook beyond the fixed
/// table of 24 per stage aborts the process.
void add_shutdown_hook(ShutdownStage stage, void (*hook)());

/// Runs every hook once, and a second call does nothing.
///
/// The first add_shutdown_hook call registers this function at exit, so a
/// process that never calls it still runs it. That registration happens after
/// the memory manager exists, so the hooks run before its exit check. A hook
/// may block and may call this function again.
void shutdown();

/// Whether shutdown() has begun running the hooks.
bool shutdown_started() noexcept;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_SHUTDOWN_H
