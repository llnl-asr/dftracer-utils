#ifndef DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H
#define DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H

#include <cstdint>

namespace dftracer::utils::duql::detail {

/// Byte index of the first occurrence of `needle` (length `needle_len`) in
/// `hay` (length `hay_len`), or -1 if absent. Vectorized (Highway dynamic
/// dispatch): the needle's first byte is broadcast across a u8 vector to find
/// candidate offsets, each verified in full. An empty needle matches at 0,
/// mirroring std::string_view::find.
std::int64_t substr_find(const char* hay, std::int64_t hay_len,
                         const char* needle, std::int64_t needle_len);

/// As substr_find, with ASCII letters compared without case. `needle` must be
/// lowercase ASCII-folded already.
std::int64_t substr_find_icase(const char* hay, std::int64_t hay_len,
                               const char* needle, std::int64_t needle_len);

}  // namespace dftracer::utils::duql::detail

#endif  // DFTRACER_UTILS_DUQL_SUBSTR_SIMD_H
