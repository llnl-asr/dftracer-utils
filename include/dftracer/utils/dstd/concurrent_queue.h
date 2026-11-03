// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_CONCURRENT_QUEUE_H
#define DFTRACER_UTILS_DSTD_CONCURRENT_QUEUE_H

#include <dftracer/utils/dstd/vendor/concurrentqueue.h>

#include <cstddef>

namespace dftracer::utils::dstd {

/**
 * @brief Traits that take the queue's blocks from the memory library.
 *
 * Blocks go to a pool of their own, "concurrent_queue" under the root, not to
 * the scope of the thread that enqueues, because a block lives until the
 * queue is destroyed and would outlive a shorter scope pool. A failed
 * allocation returns null, which the queue reports as a failed enqueue.
 */
struct ConcurrentQueueTraits : moodycamel::ConcurrentQueueDefaultTraits {
    static void* malloc(std::size_t size) noexcept;
    static void free(void* p) noexcept;
};

/// A lock-free multi-producer multi-consumer queue (moodycamel) whose memory
/// is visible to the memory budget and the exit check.
template <class T>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T, ConcurrentQueueTraits>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_CONCURRENT_QUEUE_H
