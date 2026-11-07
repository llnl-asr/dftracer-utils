// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_LIST_H
#define DFTRACER_UTILS_DSTD_LIST_H

#include <dftracer/utils/memory/allocator.h>

#include <list>

namespace dftracer::utils::dstd {

template <class T>
using List = std::list<T, memory::Allocator<T>>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_LIST_H
