#ifndef DFTRACER_UTILS_DSTD_SPAN_H
#define DFTRACER_UTILS_DSTD_SPAN_H

#include <cstddef>
#include <span>

namespace dftracer::utils::dstd {

template <class T, std::size_t Extent = std::dynamic_extent>
using Span = std::span<T, Extent>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_SPAN_H
