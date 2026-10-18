#ifndef DFTRACER_UTILS_DSTD_DEQUE_H
#define DFTRACER_UTILS_DSTD_DEQUE_H

#include <dftracer/utils/memory/allocator.h>

#include <deque>

namespace dftracer::utils::dstd {

template <class T>
using Deque = std::deque<T, memory::Allocator<T>>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_DEQUE_H
