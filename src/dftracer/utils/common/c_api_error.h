// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_C_API_ERROR_H
#define DFTRACER_UTILS_COMMON_C_API_ERROR_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/dstd/error.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::common {

dftu_status to_status(dstd::Condition condition) noexcept;

/// The condition of a negative @p status; an unknown code is Internal.
dstd::Condition to_condition(dftu_status status) noexcept;

/// Releases what @p out held and fills it from @p error, copying the message
/// (the error name when it has none).
/// A NULL @p out only maps the status. Returns the error's status.
dftu_status fill_error(dftu_error* out, const dstd::Error& error) noexcept;

/// Copies min(@p src_size, @p dst_size) bytes of the caller's struct over
/// @p dst, which holds the defaults, so a field an older caller lacks keeps
/// its default. Returns false, copying nothing, when @p src_size is below
/// @p min_size, the offset of the first field after struct_size.
bool copy_struct(void* dst, std::size_t dst_size, const void* src,
                 std::uint32_t src_size, std::size_t min_size) noexcept;

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_C_API_ERROR_H
