#ifndef DFTRACER_UTILS_MEMORY_HEAP_H
#define DFTRACER_UTILS_MEMORY_HEAP_H

#include <dftracer/utils/memory/constants.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

class Pool;

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
    /// The sum of reservations the root accepts. Zero is a third of the
    /// available memory.
    std::size_t budget = 0;
    /// Bytes of heap the process may hold from the operating system. Zero is
    /// no cap. The system backend checks it only for blocks above 1 MiB.
    std::size_t hard_cap = 0;
    /// Bytes below the process limit where under_pressure turns on. Zero is
    /// 5% of the limit and at least 256 MiB.
    std::size_t limit_headroom = 0;
    /// Bytes past the budget a reclaiming pool may reserve. Zero is 2% of the
    /// budget and at least 32 MiB.
    std::size_t spill_headroom = 0;
};

/**
 * @brief The configuration in effect.
 *
 * The backend and the decay time are fixed at the first call to any function
 * in this header and never change; constants::MEMORY_BACKEND_ENV and
 * constants::DECAY_MS_ENV pick them. The other fields hold the resolved
 * values, never zero except hard_cap.
 */
Config config() noexcept;

/**
 * @brief Set the budget, the hard cap and the two headrooms.
 *
 * Every one of those fields is applied and a zero selects its default, so a
 * default Config restores them all. The backend and the decay time are
 * ignored. Safe to call while other threads allocate.
 */
void configure(const Config& config) noexcept;

/**
 * @brief Allocate @p size bytes aligned to @p align.
 *
 * @p align must be a power of two. The page heap serves at most 2 MiB and
 * aborts on more. A zero @p size returns a unique pointer that must still be
 * freed. Throws std::bad_alloc when the memory is not available or the block
 * would pass the hard cap. The block is freed only by deallocate() of this
 * library.
 */
void* allocate(std::size_t size, std::size_t align);

/**
 * @brief Allocate like allocate() and charge the block to @p pool, as a Scope
 * of @p pool would, whatever the scope of the calling thread.
 *
 * The pool must be open.
 */
void* allocate(std::size_t size, std::size_t align, Pool& pool);

/**
 * @brief Allocate like allocate() and return null instead of throwing.
 *
 * Null means the hard cap or the operating system refused. A bad alignment
 * still aborts.
 */
void* try_allocate(std::size_t size, std::size_t align) noexcept;

/// Free a block from allocate(); null is a no-op.
void deallocate(void* p) noexcept;

/**
 * @brief Free a block from allocate() whose requested size is @p size.
 *
 * Debug builds abort when @p size differs from the size passed to allocate().
 */
void deallocate(void* p, std::size_t size) noexcept;

/**
 * @brief Free a block like deallocate(), for a coroutine frame.
 *
 * With constants::DEBUG_ENV on, the block is filled with the freed pattern
 * and its memory is not given to any other allocation until the pool the
 * block was charged to closes or the process exits, so a late resume of the
 * frame or a read through it finds the pattern. The pool counts the block
 * until then. Without the debug mode this is deallocate().
 */
void deallocate_retained(void* p, std::size_t size) noexcept;

/**
 * @brief Resize a block from allocate() whose requested size is @p old_size.
 *
 * Returns @p p when the block already fits @p new_size at @p align, else a
 * new block that holds the first min(@p old_size, @p new_size) bytes, and @p p
 * is freed. Either way the block stays charged to the pool of the old block
 * and is freed with @p new_size. A null @p p is allocate(@p new_size,
 * @p align). Throws std::bad_alloc and leaves @p p intact when the memory is
 * not available. Debug builds abort when @p old_size is not the size of the
 * block.
 */
void* reallocate(void* p, std::size_t old_size, std::size_t new_size,
                 std::size_t align);

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
