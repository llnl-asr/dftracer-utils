// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_VARIANT_H
#define DFTRACER_UTILS_DSTD_VARIANT_H

#include <variant>

namespace dftracer::utils::dstd {

/// One of several types, stored inline. It allocates nothing.
template <class... Ts>
// keep std: defines dstd::Variant
using Variant = std::variant<Ts...>;

/// The empty alternative, for a Variant that may hold nothing.
// keep std: defines dstd::Variant
using Monostate = std::monostate;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_VARIANT_H
