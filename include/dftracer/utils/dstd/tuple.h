// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_TUPLE_H
#define DFTRACER_UTILS_DSTD_TUPLE_H

#include <tuple>

namespace dftracer::utils::dstd {

template <class... Ts>
using Tuple = std::tuple<Ts...>;
/// Deduces a @ref Tuple; the alias itself does not deduce on every compiler.
using std::make_tuple;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_TUPLE_H
