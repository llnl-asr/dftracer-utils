// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_SYMBOLIZE_H
#define DFTRACER_UTILS_COMMON_SYMBOLIZE_H

#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::common {

/// Resolve a function pointer to a short, readable name: dladdr + demangle,
/// reduced to the bare function name; falls back to "module+0xoffset" and then
/// "?" when no symbol is available. Used by both the coroutine monitor and the
/// logger's coroutine tracing.
dstd::String symbolize_function(const void* fn);

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_SYMBOLIZE_H
