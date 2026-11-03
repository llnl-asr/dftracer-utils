// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_MAP_H
#define DFTRACER_UTILS_DSTD_MAP_H

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/memory/allocator.h>

#include <functional>
#include <map>
#include <set>
#include <utility>

namespace dftracer::utils::dstd {

/// The comparator is transparent, so a Map keyed by String finds by
/// std::string_view without building a key.
template <class K, class V, class Compare = std::less<>>
// keep std: defines dstd::Map
using Map = std::map<K, V, Compare, memory::Allocator<Pair<const K, V>>>;

template <class T, class Compare = std::less<>>
// keep std: defines dstd::Map
using Set = std::set<T, Compare, memory::Allocator<T>>;

template <class K, class V, class Compare = std::less<>>
using MultiMap =
    // keep std: defines dstd::Map
    std::multimap<K, V, Compare, memory::Allocator<Pair<const K, V>>>;

template <class T, class Compare = std::less<>>
// keep std: defines dstd::Map
using MultiSet = std::multiset<T, Compare, memory::Allocator<T>>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_MAP_H
