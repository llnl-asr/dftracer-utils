#ifndef DFTRACER_UTILS_MEMORY_DEBUG_H
#define DFTRACER_UTILS_MEMORY_DEBUG_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

inline constexpr std::size_t DEBUG_REDZONE_BYTES = 16;
/// Freed blocks held back with their fill pattern before the backend gets
/// them, so a write to a freed block is seen when its turn comes.
inline constexpr std::size_t DEBUG_QUARANTINE_BLOCKS = 64;
inline constexpr std::size_t DEBUG_TABLE_SLOTS = std::size_t{1} << 21;
inline constexpr std::size_t DEBUG_SITE_GROUPS = 1024;
inline constexpr std::uint8_t DEBUG_FREED_FILL = 0xdd;
inline constexpr std::uint8_t DEBUG_REDZONE_FILL = 0xfb;
inline constexpr std::uint16_t DEBUG_ALL_POOLS = 0xffff;

/// Live blocks of one pool that one caller address allocated.
struct DebugSite {
    std::uint16_t pool;
    std::uintptr_t site;
    std::size_t blocks;
    std::size_t bytes;
};

/// Called once per group; a group with site 0 sums the groups that did not fit.
using DebugVisit = void (*)(void* context, const DebugSite& group) noexcept;

/// Allocate from the active backend with a redzone behind the block and record
/// @p site, the caller address, and the size in a side table that takes no
/// memory from this heap. Aborts when the table is full.
void* debug_allocate(std::size_t size, std::size_t align, void* site);

/// Abort on a double free, a pointer this library never gave out, a @p sized
/// free of another size and a redzone that was written. The block is filled
/// and held in quarantine, and the oldest quarantined block must still hold
/// its fill or the process aborts.
void debug_deallocate(void* p, std::size_t size, bool sized) noexcept;

/// Move a block to a new one of @p new_size bytes in the pool of the old one,
/// recorded at @p site. The free of the old block is checked as a sized free
/// of @p old_size.
void* debug_reallocate(void* p, std::size_t old_size, std::size_t new_size,
                       std::size_t align, void* site);

/// Check and release every quarantined block, so pool counters match.
void debug_drain() noexcept;

/// Visit the live blocks of pool @p pool, or of every pool for
/// DEBUG_ALL_POOLS, grouped by pool and site. Call debug_drain() first.
void debug_for_each_site(std::uint16_t pool, DebugVisit visit,
                         void* context) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_DEBUG_H
