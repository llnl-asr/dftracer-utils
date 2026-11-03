// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_ALGORITHM_H
#define DFTRACER_UTILS_DSTD_ALGORITHM_H

#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <utility>

namespace dftracer::utils::dstd {

/// Sorts [first, last) and keeps the order of equal elements, as
/// std::stable_sort does. Its merge buffer comes from memory::detail::allocate,
/// where std::stable_sort takes one from operator new.
template <class It, class Compare = std::less<>>
void stable_sort(It first, It last, Compare comp = {}) {
    using T = typename std::iterator_traits<It>::value_type;
    using Diff = typename std::iterator_traits<It>::difference_type;
    constexpr Diff RUN = 32;
    const Diff n = last - first;
    if (n < 2) return;
    for (Diff lo = 0; lo < n; lo += RUN) {
        const Diff hi = std::min(lo + RUN, n);
        for (Diff i = lo + 1; i < hi; ++i) {
            T x = std::move(first[i]);
            Diff j = i;
            for (; j > lo && comp(x, first[j - 1]); --j)
                first[j] = std::move(first[j - 1]);
            first[j] = std::move(x);
        }
    }
    if (n <= RUN) return;
    Vector<T> left;
    left.reserve(static_cast<std::size_t>(n / 2 + RUN));
    for (Diff width = RUN; width < n; width *= 2) {
        for (Diff lo = 0; lo + width < n; lo += 2 * width) {
            const Diff mid = lo + width;
            const Diff hi = std::min(lo + 2 * width, n);
            if (!comp(first[mid], first[mid - 1])) continue;
            left.clear();
            std::move(first + lo, first + mid, std::back_inserter(left));
            std::size_t i = 0;
            Diff j = mid;
            Diff k = lo;
            while (i < left.size() && j < hi) {
                if (comp(first[j], left[i])) {
                    first[k++] = std::move(first[j++]);
                } else {
                    first[k++] = std::move(left[i++]);
                }
            }
            while (i < left.size()) first[k++] = std::move(left[i++]);
        }
    }
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_ALGORITHM_H
