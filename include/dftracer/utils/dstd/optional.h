// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_OPTIONAL_H
#define DFTRACER_UTILS_DSTD_OPTIONAL_H

#include <optional>

namespace dftracer::utils::dstd {

/// A value that may be absent. It owns the value inline and allocates
/// nothing, so there is no allocator to name.
template <class T>
// keep std: defines dstd::Optional
using Optional = std::optional<T>;

using NullOpt = std::nullopt_t;
// keep std: defines dstd::Optional
inline constexpr NullOpt nullopt = std::nullopt;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_OPTIONAL_H
