#ifndef DFTRACER_UTILS_MEMORY_HEAP_H
#define DFTRACER_UTILS_MEMORY_HEAP_H

#include <dftracer/utils/memory/constants.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

enum class Backend : std::uint8_t {
    PageHeap,
    /// The C library allocator with a header per block, for sanitizers and
    /// A/B runs.
    System,
};

enum class Mode : std::uint8_t {
    /// Purge only memory idle for longer than the decay time.
    Decayed,
    /// Purge all idle memory held by the shared cache, by the calling thread's
    /// heap and by heaps that no thread holds. Heaps held by other running
    /// threads are not reached: only their owner touches them, and it purges at
    /// its own slow paths or when it exits.
    All,
};

struct Config {
    Backend backend = Backend::PageHeap;
    /// Milliseconds idle memory stays backed; constants::DECAY_MS_ENV sets it.
    std::uint32_t decay_ms = constants::DEFAULT_DECAY_MS;
};

/**
 * @brief The configuration in effect.
 *
 * The backend is fixed at the first call to any function in this header and
 * never changes; constants::MEMORY_BACKEND_ENV picks it.
 */
Config config() noexcept;

/**
 * @brief Allocate @p size bytes aligned to @p align.
 *
 * @p align must be a power of two. The page heap serves at most 2 MiB and
 * aborts on more. A zero @p size returns a unique pointer
 * that must still be freed. Throws std::bad_alloc when the memory is not
 * available. The block is freed only by deallocate() of this library.
 */
void* allocate(std::size_t size, std::size_t align);

/// Free a block from allocate(); null is a no-op.
void deallocate(void* p) noexcept;

/**
 * @brief Free a block from allocate() whose requested size is @p size.
 *
 * Debug builds abort when @p size differs from the size passed to allocate().
 */
void deallocate(void* p, std::size_t size) noexcept;

/**
 * @brief Give idle memory back to the operating system.
 *
 * Purged memory stays mapped, so addresses are never reused by other code
 * and a block that is allocated again is backed again on first touch. Safe to
 * call from any thread while other threads allocate and free.
 */
void trim(Mode mode) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_HEAP_H
