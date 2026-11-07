// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H
#define DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H

#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::duql::detail {

/// Byte index of the first occurrence of `needle` (length `needle_size`) in
/// `hay` (length `hay_size`), or -1 if absent. Vectorized (Highway dynamic
/// dispatch): the needle's first byte is broadcast across a u8 vector to find
/// candidate offsets, each verified in full. An empty needle matches at 0,
/// mirroring dstd::StringView::find.
std::int64_t substr_find(const char* hay, std::int64_t hay_size,
                         const char* needle, std::int64_t needle_size);

/// As substr_find, with ASCII letters compared without case. `needle` must be
/// lowercase ASCII-folded already.
std::int64_t substr_find_icase(const char* hay, std::int64_t hay_size,
                               const char* needle, std::int64_t needle_size);

}  // namespace dftracer::utils::duql::detail

#endif  // DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H
