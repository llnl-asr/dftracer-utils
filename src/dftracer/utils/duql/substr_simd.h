// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H
#define DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H

#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::duql::detail {

/// Byte index of the first occurrence of `needle` in `hay`, or -1 if absent.
/// Vectorized (Highway dynamic dispatch): the needle's first byte is broadcast
/// across a u8 vector to find candidate offsets, each verified in full. An
/// empty needle matches at 0, mirroring dstd::StringView::find.
std::int64_t substr_find(dstd::StringView hay, dstd::StringView needle);

/// As substr_find, with ASCII letters compared without case. `needle` must be
/// lowercase ASCII-folded already.
std::int64_t substr_find_icase(dstd::StringView hay, dstd::StringView needle);

/// Offset of the first vector block of `d` that holds a byte at or above
/// 0x80, or the start of the tail shorter than a block. The vector width
/// depends on the target, so a caller scans on from there byte by byte.
std::size_t ascii_prefix(const unsigned char* d, std::size_t n);

}  // namespace dftracer::utils::duql::detail

#endif  // DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H
