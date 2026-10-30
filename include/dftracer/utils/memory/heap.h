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
    /// Purge all idle memory of the shared cache, the calling thread's heap and
    /// heaps no thread holds. Heaps of other running threads are not reached.
    /// Their owner purges them at its slow paths or when it exits.
    All,
};

struct Config {
    Backend backend = Backend::PageHeap;
    /// Milliseconds idle memory stays backed. constants::DECAY_MS_ENV sets it.
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
 * in this header. constants::MEMORY_BACKEND_ENV and constants::DECAY_MS_ENV
 * pick them. The other fields hold resolved values, never zero except hard_cap.
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
 * @p align is a power of two. The page heap aborts above 2 MiB. Zero @p size
 * still needs a free. Throws std::bad_alloc when memory is not available or the
 * hard cap would be passed. Free only with this library's deallocate().
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
 * @brief Allocate like allocate(size, align, pool), but name @p site as the
 * caller in the debug mode, for a wrapper such as the global operator new
 * whose own address says nothing.
 */
void* allocate_at(std::size_t size, std::size_t align, Pool& pool, void* site);

/**
 * @brief Allocate like allocate() and return null instead of throwing.
 *
 * Null means the hard cap or the operating system refused. A bad alignment
 * still aborts.
 */
void* try_allocate(std::size_t size, std::size_t align) noexcept;

/// Free a block from allocate(); null is a no-op.
void deallocate(void* p) noexcept;

/// Blocks handed out since the process started, a resize counted as one. Zero
/// unless the debug mode (DFTRACER_UTILS_MEMORY_DEBUG=1) is on.
std::uint64_t debug_allocation_count() noexcept;

/**
 * @brief Free a block from allocate() whose requested size is @p size.
 *
 * Debug builds abort when @p size differs from the size passed to allocate().
 */
void deallocate(void* p, std::size_t size) noexcept;

/**
 * @brief Free a block like deallocate(), for a coroutine frame.
 *
 * With constants::DEBUG_ENV on, the block is filled with the freed pattern and
 * not reused until its pool closes or the process exits. The pool counts it
 * until then. Without the debug mode this is deallocate().
 */
void deallocate_retained(void* p, std::size_t size) noexcept;

/**
 * @brief Resize a block from allocate() whose requested size is @p old_size.
 *
 * Returns @p p if it fits, else a copy of the first min(@p old_size, @p
 * new_size) bytes and frees @p p. Null @p p is allocate(). Throws
 * std::bad_alloc and leaves @p p intact. Debug builds abort on a wrong @p
 * old_size.
 */
void* reallocate(void* p, std::size_t old_size, std::size_t new_size,
                 std::size_t align);

/**
 * @brief Give idle memory back to the operating system.
 *
 * Purged memory stays mapped. Safe to call from any thread while other threads
 * allocate and free.
 */
void trim(Mode mode) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_HEAP_H
