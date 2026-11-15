// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_TRY_RESERVE_H
#define DFTRACER_UTILS_DSTD_TRY_RESERVE_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/heap.h>

#include <cstddef>
#include <limits>

namespace dftracer::utils::dstd {

namespace detail {

/// Tests that the heap can hand out @p bytes now, and gives the block back.
inline Result<void> probe_allocation(std::size_t bytes,
                                     std::size_t align) noexcept {
    void* block = memory::detail::try_allocate(bytes, align);
    if (block == nullptr) {
        return Error::from_format<"cannot allocate {} bytes">(
            Condition::OutOfMemory, bytes);
    }
    memory::detail::deallocate(block, bytes);
    return {};
}

}  // namespace detail

/**
 * @brief Reserves room for @p count elements in @p vec, or reports
 * @ref Condition::OutOfMemory.
 *
 * A plain reserve() or push_back() aborts the process when the heap refuses.
 * Call this once per batch that is sized by its input, then append without
 * further growth. The vector is not changed on failure. The check is made
 * against the heap before the vector grows, so a concurrent allocation can
 * still make the reserve itself abort.
 */
template <class T>
[[nodiscard]] Result<void> try_reserve(Vector<T>& vec, std::size_t count) {
    if (count <= vec.capacity()) return {};
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
        return Error::from_format<"cannot allocate {} elements">(
            Condition::OutOfMemory, count);
    }
    DFTRACER_UTILS_TRY_VOID(
        detail::probe_allocation(count * sizeof(T), alignof(T)));
    vec.reserve(count);
    return {};
}

/// Like the vector overload, for @p count characters and the NUL.
[[nodiscard]] inline Result<void> try_reserve(String& text, std::size_t count) {
    if (count <= text.capacity()) return {};
    if (count >= std::numeric_limits<std::size_t>::max() / sizeof(char)) {
        return Error::from_format<"cannot allocate {} characters">(
            Condition::OutOfMemory, count);
    }
    DFTRACER_UTILS_TRY_VOID(
        detail::probe_allocation((count + 1) * sizeof(char), alignof(char)));
    text.reserve(count);
    return {};
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_TRY_RESERVE_H
