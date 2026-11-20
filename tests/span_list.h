// SPDX-License-Identifier: MIT
#ifndef DFTU_TESTS_SPAN_LIST_H
#define DFTU_TESTS_SPAN_LIST_H

#include <dftracer/utils/dstd/span.h>

#include <initializer_list>

namespace dftu_utils_test {

/// A braced list as a read-only span, as C++26 gives std::span. The span views
/// the backing array of the list, which lives to the end of the full
/// expression that holds the call, so pass it straight to a function.
template <class T>
dftracer::utils::dstd::Span<const T> span_of(std::initializer_list<T> items) {
    return {items.begin(), items.size()};
}

}  // namespace dftu_utils_test

#endif  // DFTU_TESTS_SPAN_LIST_H
