#ifndef DFTRACER_UTILS_CORE_COMMON_SPAN_H
#define DFTRACER_UTILS_CORE_COMMON_SPAN_H

#include <nonstd/span.hpp>

namespace dftracer {
namespace utils {
template <typename T, std::size_t Extent = nonstd::dynamic_extent>
using span_view = nonstd::span<T, Extent>;
}  // namespace utils
}  // namespace dftracer

#endif  // DFTRACER_UTILS_CORE_COMMON_SPAN_H
