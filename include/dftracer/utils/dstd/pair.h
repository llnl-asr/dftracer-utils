// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_PAIR_H
#define DFTRACER_UTILS_DSTD_PAIR_H

#include <utility>

namespace dftracer::utils::dstd {

template <class A, class B>
using Pair = std::pair<A, B>;
/// Deduces a @ref Pair; the alias itself does not deduce on every compiler.
using std::make_pair;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_PAIR_H
