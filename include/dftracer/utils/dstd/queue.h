// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_QUEUE_H
#define DFTRACER_UTILS_DSTD_QUEUE_H

#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/vector.h>

#include <functional>
#include <queue>

namespace dftracer::utils::dstd {

template <class T>
using Queue = std::queue<T, Deque<T>>;

/// The largest element by `Compare` is on top, as in std::priority_queue.
template <class T, class Compare = std::less<T>>
using PriorityQueue = std::priority_queue<T, Vector<T>, Compare>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_QUEUE_H
