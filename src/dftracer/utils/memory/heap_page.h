// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_HEAP_PAGE_H
#define DFTRACER_UTILS_MEMORY_HEAP_PAGE_H

#include <dftracer/utils/memory/detail/heap.h>

#include <cstddef>

namespace dftracer::utils::memory::detail {

inline constexpr std::size_t HEAP_PAGE_SIZE = std::size_t{64} << 10;
inline constexpr std::size_t HEAP_SPAN_SIZE = std::size_t{4} << 20;
inline constexpr std::size_t SMALL_MAX = std::size_t{32} << 10;
inline constexpr std::size_t SMALL_MAX_ALIGN = 64;
inline constexpr std::size_t MEDIUM_MAX = std::size_t{1} << 20;
inline constexpr std::size_t LARGE_MAX = std::size_t{64} << 20;
/// A freed large run of at least this size is purged at once and cached cold.
/// A doubling container leaves old runs in classes it never asks for again, and
/// decay runs only when a heap maps or frees a large run. 2 and 4 MiB slow the
/// mixed heap_bench case.
inline constexpr std::size_t LARGE_PURGE_SIZE = std::size_t{8} << 20;
/// Freed but still backed bytes past which a heap purges its idle pages on
/// free, so retention does not grow with the free rate or the thread count.
/// 64 MiB made an index build fault its buffers back far more often and
/// spend much of its time in the kernel at high thread counts.
inline constexpr std::size_t IDLE_BACKED_CAP = std::size_t{1} << 30;
/// The largest alignment the page heap serves. A block aligned more would sit
/// in a span other than the one that holds its header.
inline constexpr std::size_t MAX_ALIGN = HEAP_SPAN_SIZE / 2;

/// Live and reserved bytes summed over every heap. Exact only while no thread
/// allocates or frees. A block freed by another thread is credited to the heap
/// that freed it, so one heap alone can read below zero.
struct PageStats {
    /// Usable bytes of the blocks not yet freed. The class size for a small
    /// block, the run size for the others.
    std::size_t live;
    /// Bytes mapped from the operating system.
    std::size_t reserved;
};

void* page_allocate(std::size_t size, std::size_t align);
/// Like page_allocate but charges the block to @p pool whatever the scope.
void* page_allocate_to(std::size_t size, std::size_t align, std::uint16_t pool);
/// Like page_allocate and page_allocate_to, but null when the memory is not
/// available. The others end the process.
void* page_try_allocate(std::size_t size, std::size_t align);
void* page_try_allocate_to(std::size_t size, std::size_t align,
                           std::uint16_t pool);
void page_deallocate(void* p) noexcept;
void page_deallocate(void* p, std::size_t size) noexcept;
void page_trim(Mode mode) noexcept;
void page_trim_thread() noexcept;

/// Move a huge block to @p size bytes, which must be larger than LARGE_MAX and
/// than the block. The contents stay. Returns null and leaves the block intact
/// when the memory is not available.
void* page_grow_huge(void* p, std::size_t size, std::size_t align);

/// Resize a block from page_allocate as reallocate() describes. Ends the
/// process when the memory is not available.
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
/// Heaps ever created. A heap is never unmapped and an exited thread's heap is
/// reused by the next thread.
std::size_t page_heap_count() noexcept;
bool page_checks_enabled() noexcept;

std::size_t page_class_count() noexcept;
std::size_t page_class_size(std::size_t cls) noexcept;
/// The small class that serves @p size at @p align, as page_allocate picks it.
std::size_t page_class_for(std::size_t size, std::size_t align) noexcept;

}  // namespace dftracer::utils::memory::detail

#endif  // DFTRACER_UTILS_MEMORY_HEAP_PAGE_H
