#ifndef DFTRACER_UTILS_MEMORY_HEAP_PAGE_H
#define DFTRACER_UTILS_MEMORY_HEAP_PAGE_H

#include <dftracer/utils/memory/heap.h>

#include <cstddef>

namespace dftracer::utils::memory {

inline constexpr std::size_t HEAP_PAGE_SIZE = std::size_t{64} << 10;
inline constexpr std::size_t HEAP_SPAN_SIZE = std::size_t{4} << 20;
inline constexpr std::size_t SMALL_MAX = std::size_t{32} << 10;
inline constexpr std::size_t SMALL_MAX_ALIGN = 64;
inline constexpr std::size_t MEDIUM_MAX = std::size_t{1} << 20;
inline constexpr std::size_t LARGE_MAX = std::size_t{64} << 20;
/// A freed large run of at least this size is purged at once and cached cold,
/// as jemalloc purges huge extents eagerly when it runs without background
/// threads: a doubling container leaves its old runs in classes it never asks
/// for again, and decay runs only when a heap maps or frees a large run.
/// 2 and 4 MiB slow the mixed heap_bench case by 4 to 6% (reuse faults again).
inline constexpr std::size_t LARGE_PURGE_SIZE = std::size_t{8} << 20;
/// The largest alignment the page heap serves. A block that is aligned more
/// would sit in a span other than the one that holds its header.
inline constexpr std::size_t MAX_ALIGN = HEAP_SPAN_SIZE / 2;

/// Live and reserved bytes summed over every heap. Exact only while no thread
/// allocates or frees; a block freed by another thread is credited to the heap
/// that freed it, so one heap alone can read below zero.
struct PageStats {
    /// Usable bytes of the blocks not yet freed: the class size for a small
    /// block, the run size for the others.
    std::size_t live;
    /// Bytes mapped from the operating system.
    std::size_t reserved;
};

void* page_allocate(std::size_t size, std::size_t align);
void page_deallocate(void* p) noexcept;
void page_deallocate(void* p, std::size_t size) noexcept;
void page_trim(Mode mode) noexcept;

/// Move a huge block to @p size bytes, which must be larger than LARGE_MAX
/// and larger than the block. The contents stay. Linux grows the mapping
/// with mremap; elsewhere the block is copied. Throws std::bad_alloc.
void* page_grow_huge(void* p, std::size_t size, std::size_t align);

/// Resize a block from page_allocate as reallocate() describes.
void* page_reallocate(void* p, std::size_t old_size, std::size_t new_size,
                      std::size_t align);

PageStats page_stats() noexcept;
/// The reserved field of page_stats() alone, without walking the pools.
std::size_t page_reserved() noexcept;
/// The most mapped bytes seen at a map from the operating system, counting the
/// bytes being mapped.
std::size_t page_reserved_peak() noexcept;
/// The most live bytes seen, read each time the bytes mapped so far pass
/// another USED_SAMPLE_BYTES, so it can trail the true peak by that much.
std::size_t page_used_peak() noexcept;
/// Backed bytes purged so far because a map would have passed the budget.
std::size_t page_budget_purged() noexcept;
/// Push the calling thread's buffered remote frees to their pages.
void page_flush_remote() noexcept;
/// Heaps ever created. A heap is never unmapped; an exited thread's heap is
/// reused by the next thread.
std::size_t page_heap_count() noexcept;
bool page_checks_enabled() noexcept;

std::size_t page_class_count() noexcept;
std::size_t page_class_size(std::size_t cls) noexcept;
/// The small class that serves @p size at @p align, as page_allocate picks it.
std::size_t page_class_for(std::size_t size, std::size_t align) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_HEAP_PAGE_H
