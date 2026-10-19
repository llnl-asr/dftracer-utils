#ifndef DFTRACER_UTILS_DSTD_VECTOR_H
#define DFTRACER_UTILS_DSTD_VECTOR_H

#include <dftracer/utils/memory/allocator.h>

#include <vector>

namespace dftracer::utils::dstd {

template <class T>
using Vector = std::vector<T, memory::Allocator<T>>;

/// Charged to the pool its allocator names, on any thread.
template <class T>
using PoolVector = std::vector<T, memory::PoolAllocator<T>>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_VECTOR_H
