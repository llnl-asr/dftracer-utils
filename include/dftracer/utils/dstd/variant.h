// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_VARIANT_H
#define DFTRACER_UTILS_DSTD_VARIANT_H

#include <variant>

namespace dftracer::utils::dstd {

/// One of several types, stored inline. It allocates nothing.
template <class... Ts>
using Variant = std::variant<Ts...>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_VARIANT_H
