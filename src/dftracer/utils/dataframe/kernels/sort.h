#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_SORT_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_SORT_H

#include <dftracer/utils/core/common/hash/splitmix64.h>
#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <hwy/contrib/sort/vqsort.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace dftracer::utils::dataframe {

/// Three-way order of rows `a` and `b` of `v` (-1/0/1) on the column's own
/// domain. Only defined for an is_orderable_type() column; 0 for any other.
int compare_rows(const Series& v, std::int64_t a, std::int64_t b);

/// Stable argsort: an Int64 column of row indices ordering `v` ascending, or
/// descending when `descending`. Null rows sort last in both directions.
/// An invalid Series for a nested column, which has no order.
Series argsort(const Series& v, bool descending);

/// An Int64 column of the k row indices of the k largest (or smallest) values,
/// ordered best-first (partial sort). k is clamped to [0, length].
Series topk_indices(const Series& v, std::int64_t k, bool largest);

// highway's vqsort with the empty array ruled out: where vqsort is disabled
// (every SVE target, since HWY_HAVE_SCALABLE) it falls back to a heap sort
// whose build-heap index underflows when it is handed no keys at all, and
// spins forever. Nothing below two keys needs sorting anyway.
template <class T>
void vqsort(T* data, std::size_t n) {
    if (n < 2) return;
    hwy::VQSort(data, n, hwy::SortAscending());
}

/// Parallel full sort of `n` values in place: SIMD-sort (VQSort) runs per chunk
/// in parallel, then a bottom-up parallel merge. Falls back to a single VQSort
/// when no backend is installed (the merge would be pure overhead) or `n` is
/// small. `less` must match VQSort's ascending order so the merge is
/// consistent, and `T` must be a type VQSort supports (built-in numeric types,
/// or a packed key/index type such as hwy::uint128_t).
/// Sample sort: bucket each value by its key against sorted splitters drawn
/// from a sample of the keys, then VQSort every bucket in parallel. One
/// parallel pass assigns buckets and counts them, one scatters into scratch,
/// the bucket sorts run in parallel, one pass copies back, and there is no
/// merge phase; the result is left in `sorted`, not copied back over
/// `data`. Sampled splitters balance the buckets for any distribution
/// (a float's bit pattern crowds its exponent bits; equal-width slices of
/// the key range would put half the data in one bucket). A value in bucket
/// b sorts before one in b+1, so the buckets concatenate in order, and each
/// bucket's VQSort by the full word keeps ties stable. Falls back (returns
/// false) when the keys still crowd one bucket (many equal keys), since a
/// single huge bucket would be one serial sort.
template <class T, class KeyOf>
bool bucket_packed_sort(T* data, std::size_t n, std::size_t buckets,
                        KeyOf key_of, Scratch<T>& sorted) {
    constexpr std::size_t GRAIN = std::size_t{1} << 16;
    constexpr std::size_t OVERSAMPLE = 16;
    std::vector<std::uint64_t> sample;
    sample.reserve(buckets * OVERSAMPLE);
    for (std::size_t i = 0; i < buckets * OVERSAMPLE; ++i)
        sample.push_back(
            key_of(data[dftracer::utils::hash::splitmix64(i) % n]));
    std::sort(sample.begin(), sample.end());
    std::vector<std::uint64_t> splitters;  // buckets - 1 of them, ascending
    splitters.reserve(buckets - 1);
    for (std::size_t k = 1; k < buckets; ++k)
        splitters.push_back(sample[k * OVERSAMPLE]);
    auto bucket_of = [&](const T& v) {
        const std::uint64_t k = key_of(v);
        return static_cast<std::uint16_t>(
            std::upper_bound(splitters.begin(), splitters.end(), k) -
            splitters.begin());
    };

    const std::size_t chunks = (n + GRAIN - 1) / GRAIN;
    std::vector<std::size_t> hist(chunks * buckets, 0);
    Scratch<std::uint16_t> which(n);  // each value's bucket, found once
    parallel_for(static_cast<std::int64_t>(n), static_cast<std::int64_t>(GRAIN),
                 [&](std::int64_t b, std::int64_t e) {
                     std::size_t* h =
                         hist.data() +
                         (static_cast<std::size_t>(b) / GRAIN) * buckets;
                     for (std::int64_t i = b; i < e; ++i) {
                         const std::uint16_t w =
                             bucket_of(data[static_cast<std::size_t>(i)]);
                         which[static_cast<std::size_t>(i)] = w;
                         ++h[w];
                     }
                 });
    // Bucket starts, and per chunk the write cursor into each bucket
    // (column-major prefix: bucket by bucket, chunk by chunk within it).
    std::vector<std::size_t> start(buckets + 1, 0);
    std::size_t largest = 0;
    for (std::size_t k = 0; k < buckets; ++k) {
        std::size_t total = 0;
        for (std::size_t c = 0; c < chunks; ++c) {
            const std::size_t cnt = hist[c * buckets + k];
            hist[c * buckets + k] = start[k] + total;
            total += cnt;
        }
        start[k + 1] = start[k] + total;
        largest = std::max(largest, total);
    }
    if (largest > n / 4) return false;
    // The sorted order lands in `sorted`, which the caller reads; copying
    // it back over `data` would be one more pass over everything.
    Scratch<T>& scratch = sorted;
    scratch.resize(n);
    parallel_for(static_cast<std::int64_t>(n), static_cast<std::int64_t>(GRAIN),
                 [&](std::int64_t b, std::int64_t e) {
                     std::size_t* cur =
                         hist.data() +
                         (static_cast<std::size_t>(b) / GRAIN) * buckets;
                     for (std::int64_t i = b; i < e; ++i)
                         scratch[cur[which[static_cast<std::size_t>(i)]]++] =
                             data[static_cast<std::size_t>(i)];
                 });
    parallel_for(
        static_cast<std::int64_t>(buckets), 1,
        [&](std::int64_t k0, std::int64_t k1) {
            for (std::int64_t k = k0; k < k1; ++k) {
                const std::size_t lo = start[static_cast<std::size_t>(k)];
                const std::size_t hi = start[static_cast<std::size_t>(k) + 1];
                if (hi > lo) vqsort(scratch.data() + lo, hi - lo);
            }
        });
    return true;
}

template <class T, class Less>
void parallel_packed_sort(T* data, std::size_t n, Less less) {
    constexpr std::size_t RUN = std::size_t{1} << 18;
    if (!parallel_backend_installed() || n <= RUN) {
        vqsort(data, n);
        return;
    }
    const std::size_t nruns = (n + RUN - 1) / RUN;
    parallel_for(static_cast<std::int64_t>(nruns), 1,
                 [&](std::int64_t c0, std::int64_t c1) {
                     for (std::int64_t c = c0; c < c1; ++c) {
                         const std::size_t lo =
                             static_cast<std::size_t>(c) * RUN;
                         const std::size_t hi = std::min(n, lo + RUN);
                         vqsort(data + lo, hi - lo);
                     }
                 });
    std::vector<T> scratch(n);
    T* src = data;
    T* dst = scratch.data();
    for (std::size_t width = RUN; width < n; width *= 2) {
        const std::size_t step = width * 2;
        const std::size_t npairs = (n + step - 1) / step;
        // Every merge is cut into RUN-sized output segments (merge path: a
        // binary search finds where each segment starts in both runs), so
        // the last levels, one wide merge each, still fill the pool.
        const std::size_t segs = (step + RUN - 1) / RUN;
        parallel_for(
            static_cast<std::int64_t>(npairs * segs), 1,
            [&](std::int64_t t0, std::int64_t t1) {
                for (std::int64_t t = t0; t < t1; ++t) {
                    const std::size_t p = static_cast<std::size_t>(t) / segs;
                    const std::size_t sgm = static_cast<std::size_t>(t) % segs;
                    const std::size_t lo = p * step;
                    if (lo >= n) continue;
                    const std::size_t mid = std::min(n, lo + width);
                    const std::size_t hi = std::min(n, lo + step);
                    const std::size_t len = hi - lo;
                    const std::size_t k0 = std::min(len, sgm * RUN);
                    const std::size_t k1 = std::min(len, k0 + RUN);
                    if (k0 >= k1) continue;
                    const T* a = src + lo;
                    const std::size_t na = mid - lo;
                    const T* b = src + mid;
                    const std::size_t nb = hi - mid;
                    // The co-rank of output position k: the split (i, k - i)
                    // with a[i-1] <= b[k-i] and b[k-i-1] < a[i], stable
                    // (ties take from a first).
                    auto corank = [&](std::size_t k) {
                        std::size_t ilo = k > nb ? k - nb : 0;
                        std::size_t ihi = std::min(k, na);
                        while (ilo < ihi) {
                            const std::size_t i = ilo + (ihi - ilo) / 2;
                            const std::size_t j = k - i;
                            if (less(b[j - 1], a[i]))
                                ihi = i;
                            else
                                ilo = i + 1;
                        }
                        return ilo;
                    };
                    const std::size_t i0 = corank(k0), i1 = corank(k1);
                    const std::size_t j0 = k0 - i0, j1 = k1 - i1;
                    std::merge(a + i0, a + i1, b + j0, b + j1, dst + lo + k0,
                               less);
                }
            });
        std::swap(src, dst);
    }
    if (src != data) std::copy(src, src + n, data);
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_SORT_H
