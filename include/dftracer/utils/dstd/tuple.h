// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_TUPLE_H
#define DFTRACER_UTILS_DSTD_TUPLE_H

#include <tuple>

namespace dftracer::utils::dstd {

template <class... Ts>
// keep std: defines dstd::Tuple
using Tuple = std::tuple<Ts...>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_TUPLE_H
