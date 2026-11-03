// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FUTURE_H
#define DFTRACER_UTILS_DSTD_FUTURE_H

#include <dftracer/utils/memory/allocator.h>

#include <future>
#include <memory>
#include <utility>

namespace dftracer::utils::dstd {

template <class T>
// keep std: defines dstd::Future
using Future = std::future<T>;

template <class T>
// keep std: defines dstd::Future
using SharedFuture = std::shared_future<T>;

/// A std::promise whose shared state comes from memory::detail::allocate, so
/// the state counts in the memory budget.
template <class T>
// keep std: defines dstd::Future
class Promise : public std::promise<T> {
   public:
    Promise()
        // keep std: defines dstd::Future
        : std::promise<T>(std::allocator_arg, memory::Allocator<char>{}) {}
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FUTURE_H
