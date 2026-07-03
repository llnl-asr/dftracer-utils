#ifndef DFTRACER_UTILS_CORE_COMMON_SYMBOLIZE_H
#define DFTRACER_UTILS_CORE_COMMON_SYMBOLIZE_H

#include <string>

namespace dftracer::utils {

// Resolve a function pointer to a short, readable name: dladdr + demangle,
// reduced to the bare function name; falls back to "module+0xoffset" and then
// "?" when no symbol is available. Used by both the coroutine monitor and the
// logger's coroutine tracing.
std::string symbolize_function(const void* fn);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SYMBOLIZE_H
