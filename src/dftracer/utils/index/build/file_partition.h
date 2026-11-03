// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_FILE_PARTITION_H
#define DFTRACER_UTILS_INDEX_BUILD_FILE_PARTITION_H

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/queue.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/filesystem/types.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace dftracer::utils::index::build {

/// Greedy Longest-Processing-Time-first (LPT) bin-packing of files into
/// `num_workers` partitions, minimising the maximum per-worker total size.
///
/// Used to eliminate straggler tails in the distributed indexer: workers
/// see file lists whose total bytes are as balanced as possible.
///
/// Complexity: O(N log N) for the initial sort + O(N log K) for the
/// min-heap over K = num_workers. `files` is consumed.
inline dstd::Vector<dstd::Vector<utilities::filesystem::FileEntry>>
plan_lpt_partition(dstd::Vector<utilities::filesystem::FileEntry> files,
                   std::size_t num_workers) {
    if (num_workers == 0) num_workers = 1;

    dstd::Vector<dstd::Vector<utilities::filesystem::FileEntry>> buckets(
        num_workers);
    if (files.empty()) return buckets;

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.size > b.size; });

    // Min-heap of (total_size, bucket_idx): next file goes to the currently
    // lightest bucket.
    using HeapEntry = dstd::Pair<std::size_t, std::size_t>;
    dstd::PriorityQueue<HeapEntry, std::greater<>> heap;
    for (std::size_t i = 0; i < num_workers; ++i) {
        heap.emplace(0, i);
    }

    for (auto& entry : files) {
        auto [total, idx] = heap.top();
        heap.pop();
        total += entry.size;
        buckets[idx].push_back(std::move(entry));
        heap.emplace(total, idx);
    }

    return buckets;
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_FILE_PARTITION_H
