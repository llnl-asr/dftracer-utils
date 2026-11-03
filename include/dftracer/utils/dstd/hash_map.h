// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_HASH_MAP_H
#define DFTRACER_UTILS_DSTD_HASH_MAP_H

#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/pair.h>
// keep std: the vendored table backs dstd::HashMap
#include <dftracer/utils/dstd/vendor/unordered_dense.h>
#include <dftracer/utils/memory/allocator.h>

#include <functional>
#include <utility>

namespace dftracer::utils::dstd {

/// The equality is transparent, so with a transparent hasher such as
/// Hash<String> the table finds by std::string_view.
template <class K, class V, class H = Hash<K>, class Eq = Equal<K>>
using HashMap =
    // keep std: the vendored table backs dstd::HashMap
    ankerl::unordered_dense::map<K, V, H, Eq, memory::Allocator<Pair<K, V>>>;

template <class T, class H = Hash<T>, class Eq = Equal<T>>
// keep std: the vendored table backs dstd::HashMap
using HashSet = ankerl::unordered_dense::set<T, H, Eq, memory::Allocator<T>>;

/// Charged to the pool their allocator names, on any thread.
template <class K, class V, class H = Hash<K>, class Eq = Equal<K>>
using PoolHashMap =
    // keep std: the vendored table backs dstd::HashMap
    ankerl::unordered_dense::map<K, V, H, Eq,
                                 memory::PoolAllocator<Pair<K, V>>>;

template <class T, class H = Hash<T>, class Eq = Equal<T>>
using PoolHashSet =
    // keep std: the vendored table backs dstd::HashMap
    ankerl::unordered_dense::set<T, H, Eq, memory::PoolAllocator<T>>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_HASH_MAP_H
