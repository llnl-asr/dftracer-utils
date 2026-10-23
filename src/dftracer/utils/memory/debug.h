#ifndef DFTRACER_UTILS_MEMORY_DEBUG_H
#define DFTRACER_UTILS_MEMORY_DEBUG_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

inline constexpr std::size_t DEBUG_REDZONE_BYTES = 16;
/// Freed blocks held back with their fill pattern before the backend gets
/// them, so a write to a freed block is seen when its turn comes.
inline constexpr std::size_t DEBUG_QUARANTINE_BLOCKS = 64;
/// One slot per address a block ever had, 32 bytes each, mapped without
/// reserve: only the touched pages are backed. With the global operator new
/// every small object has a slot.
inline constexpr std::size_t DEBUG_TABLE_SLOTS = std::size_t{1} << 27;
inline constexpr std::size_t DEBUG_SITE_GROUPS = 1024;
/// Callers the per-site totals can hold.
inline constexpr std::size_t DEBUG_SITE_TOTALS = std::size_t{1} << 16;
inline constexpr std::uint8_t DEBUG_FREED_FILL = 0xdd;
inline constexpr std::uint8_t DEBUG_REDZONE_FILL = 0xfb;
inline constexpr std::uint16_t DEBUG_ALL_POOLS = 0xffff;
/// Retained blocks the process can hold; the array is reserved, not backed.
inline constexpr std::size_t DEBUG_HELD_BLOCKS = std::size_t{1} << 24;

/// Live blocks of one pool that one caller address allocated.
struct DebugSite {
    std::uint16_t pool;
    std::uintptr_t site;
    std::size_t blocks;
    std::size_t bytes;
};

/// Called once per group; a group with site 0 sums the groups that did not fit.
using DebugVisit = void (*)(void* context, const DebugSite& group) noexcept;

/// The allocations of one caller address since the process started.
struct DebugSiteTotal {
    std::uintptr_t site;
    std::size_t live;
    std::size_t peak;
    std::size_t allocations;
};

using DebugTotalVisit = void (*)(void* context,
                                 const DebugSiteTotal& total) noexcept;

/// Visit the @p count callers (at most 64) with the most live bytes, or with
/// the highest peak of live bytes when @p by_peak, largest first. Callers
/// past the table capacity are not counted.
void debug_top_sites(std::size_t count, bool by_peak, DebugTotalVisit visit,
                     void* context) noexcept;

/// Blocks above this size count as big in the peak summary.
inline constexpr std::size_t DEBUG_BIG_BLOCK_BYTES = std::size_t{1} << 20;
/// A snapshot of the per-site live bytes is taken when the total passes the
/// last snapshot by this share, in percent, or by this many bytes, whichever
/// is larger.
inline constexpr std::size_t DEBUG_PEAK_PERCENT = 1;
inline constexpr std::size_t DEBUG_PEAK_MIN_STEP = std::size_t{16} << 20;

/// The state of the last snapshot of the live bytes.
struct DebugPeak {
    /// Live bytes of all sites when the snapshot was taken.
    std::size_t total;
    /// Part of total in blocks above DEBUG_BIG_BLOCK_BYTES.
    std::size_t big;
};

/// Visit the @p count callers (at most 64) with the most live bytes in the
/// last snapshot, largest first; each total has the bytes at the snapshot as
/// live. The snapshot trails the true peak by at most the step above.
DebugPeak debug_top_sites_at_peak(std::size_t count, DebugTotalVisit visit,
                                  void* context) noexcept;

/// Allocate from the active backend with a redzone behind the block and record
/// @p site, the caller address, and the size in a side table that takes no
/// memory from this heap. Aborts when the table is full.
void* debug_allocate(std::size_t size, std::size_t align, void* site);

/// Abort on a double free, a pointer this library never gave out, a @p sized
/// free of another size and a redzone that was written. The block is filled
/// and held in quarantine, and the oldest quarantined block must still hold
/// its fill or the process aborts.
void debug_deallocate(void* p, std::size_t size, bool sized) noexcept;

/// Free like a sized debug_deallocate(), but keep the block out of reuse in
/// the held list until debug_drain_held() releases it. Aborts when the list is
/// full.
void debug_retain(void* p, std::size_t size) noexcept;

/// Check and release the retained blocks charged to pool @p pool, or all of
/// them for DEBUG_ALL_POOLS.
void debug_drain_held(std::uint16_t pool) noexcept;

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
