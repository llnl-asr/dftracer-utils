#ifndef DFTRACER_UTILS_CORE_COMMON_TRACKED_BYTES_H
#define DFTRACER_UTILS_CORE_COMMON_TRACKED_BYTES_H

#include <atomic>
#include <cstdint>

namespace dftracer::utils::tracked_bytes {

/// Process-wide gauges for memory measurement, built only with the CMake
/// option DFTRACER_UTILS_TRACKED_BYTES. Without it every call below compiles
/// to nothing. They change no behavior and are removed when the holders move
/// to pools.

/// Resident bytes the last `reclaim_chain` pass summed over its lazy plan.
/// A store from whichever plan ran last, not a sum over plans.
std::atomic<std::uint64_t>& lazy_resident() noexcept;

/// Bytes reserved now across every MemoryPool.
std::atomic<std::uint64_t>& pool_used() noexcept;

#ifdef DFTRACER_UTILS_TRACKED_BYTES
inline void add_pool(std::uint64_t n) noexcept {
    pool_used().fetch_add(n, std::memory_order_relaxed);
}
inline void sub_pool(std::uint64_t n) noexcept {
    pool_used().fetch_sub(n, std::memory_order_relaxed);
}
inline void set_lazy(std::uint64_t total) noexcept {
    lazy_resident().store(total, std::memory_order_relaxed);
}
#else
inline void add_pool(std::uint64_t) noexcept {}
inline void sub_pool(std::uint64_t) noexcept {}
inline void set_lazy(std::uint64_t) noexcept {}
#endif

}  // namespace dftracer::utils::tracked_bytes

#endif  // DFTRACER_UTILS_CORE_COMMON_TRACKED_BYTES_H
