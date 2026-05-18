#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_FILE_PARTITION_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_FILE_PARTITION_H

#include <dftracer/utils/utilities/filesystem/types.h>

#include <algorithm>
#include <cstddef>
#include <queue>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::indexer {

/// Greedy Longest-Processing-Time-first (LPT) bin-packing of files into
/// `num_workers` partitions, minimising the maximum per-worker total size.
///
/// Used to eliminate straggler tails in the distributed indexer: workers
/// see file lists whose total bytes are as balanced as possible.
///
/// Complexity: O(N log N) for the initial sort + O(N log K) for the
/// min-heap over K = num_workers. `files` is consumed.
inline std::vector<std::vector<filesystem::FileEntry>> plan_lpt_partition(
    std::vector<filesystem::FileEntry> files, std::size_t num_workers) {
    if (num_workers == 0) num_workers = 1;

    std::vector<std::vector<filesystem::FileEntry>> buckets(num_workers);
    if (files.empty()) return buckets;

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.size > b.size; });

    // Min-heap of (total_size, bucket_idx): next file goes to the currently
    // lightest bucket.
    using HeapEntry = std::pair<std::size_t, std::size_t>;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<>> heap;
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

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_FILE_PARTITION_H
