#ifndef DFTRACER_UTILS_DSTD_OPTIONAL_H
#define DFTRACER_UTILS_DSTD_OPTIONAL_H

#include <optional>

namespace dftracer::utils::dstd {

/// A value that may be absent. It owns the value inline and allocates
/// nothing, so there is no allocator to name.
template <class T>
using Optional = std::optional<T>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_OPTIONAL_H
