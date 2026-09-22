#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_RADIX_DEDUP_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_RADIX_DEDUP_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <cstdint>
#include <string>
#include <vector>

// Shared radix-partitioned dedup helpers for the EAGER batch ops, mirroring
// the pattern already proven in the lazy cursors (lazyframe.cpp
// UniqueCursor/IsDupCursor): partition rows by key hash into disjoint
// buckets, each probed by exactly one worker with its own hash set/map (no
// locks), then combine. Header-only so eager ops in batch_ops.cpp/select.cpp
// can call it without a new translation unit.
namespace dftracer::utils::dataframe {

/// Fan-out width for the partitioned dedup below.
constexpr std::size_t RADIX_DEDUP_PARTITIONS = 32;

/// Below this row count, the partition/fan-out bookkeeping costs more than the
/// serial hash-set/map it would replace.
constexpr std::int64_t RADIX_DEDUP_MIN_ROWS = 1 << 14;

/// Radix-partition positions [0, m) by hash(key_of(j)) into
/// `RADIX_DEDUP_PARTITIONS` buckets, each with its own hash set, and mark
/// keep[j] the first time each key is seen (order-preserving): buckets never
/// overlap, so each is probed by exactly one worker with no lock. Falls back
/// to one serial set when no parallel backend is installed or `m` is below
/// the fan-out threshold.
template <class Key, class KeyOf>
std::vector<std::uint8_t> radix_first_seen_by(std::int64_t m, KeyOf&& key_of) {
    std::vector<std::uint8_t> keep(static_cast<std::size_t>(m), 0);
    if (!parallel_backend_installed() || m < RADIX_DEDUP_MIN_ROWS) {
        ankerl::unordered_dense::set<Key> seen;
        seen.reserve(static_cast<std::size_t>(m));
        for (std::int64_t j = 0; j < m; ++j)
            if (seen.insert(key_of(j)).second)
                keep[static_cast<std::size_t>(j)] = 1;
        return keep;
    }
    const std::size_t p = RADIX_DEDUP_PARTITIONS;
    std::vector<std::vector<std::int64_t>> buckets(p);
    std::hash<Key> hasher;
    for (std::int64_t j = 0; j < m; ++j)
        buckets[hasher(key_of(j)) % p].push_back(j);
    parallel_for(
        static_cast<std::int64_t>(p), 1, [&](std::int64_t pb, std::int64_t pe) {
            for (std::int64_t g = pb; g < pe; ++g) {
                ankerl::unordered_dense::set<Key> seen;
                for (std::int64_t j : buckets[static_cast<std::size_t>(g)])
                    if (seen.insert(key_of(j)).second)
                        keep[static_cast<std::size_t>(j)] = 1;
            }
        });
    return keep;
}

/// Radix-partition positions [0, m) by hash(key_of(j)) into
/// `RADIX_DEDUP_PARTITIONS` buckets, each with its own count map, and return
/// the occurrence count of each position's key. Falls back to one serial map
/// below the fan-out threshold.
template <class Key, class KeyOf>
std::vector<std::int64_t> radix_counts_by(std::int64_t m, KeyOf&& key_of) {
    std::vector<std::int64_t> counts(static_cast<std::size_t>(m), 0);
    if (!parallel_backend_installed() || m < RADIX_DEDUP_MIN_ROWS) {
        ankerl::unordered_dense::map<Key, std::int64_t> cnt;
        cnt.reserve(static_cast<std::size_t>(m));
        for (std::int64_t j = 0; j < m; ++j) ++cnt[key_of(j)];
        for (std::int64_t j = 0; j < m; ++j)
            counts[static_cast<std::size_t>(j)] = cnt[key_of(j)];
        return counts;
    }
    const std::size_t p = RADIX_DEDUP_PARTITIONS;
    std::vector<std::vector<std::int64_t>> buckets(p);
    std::hash<Key> hasher;
    for (std::int64_t j = 0; j < m; ++j)
        buckets[hasher(key_of(j)) % p].push_back(j);
    parallel_for(
        static_cast<std::int64_t>(p), 1, [&](std::int64_t pb, std::int64_t pe) {
            for (std::int64_t g = pb; g < pe; ++g) {
                ankerl::unordered_dense::map<Key, std::int64_t> cnt;
                for (std::int64_t j : buckets[static_cast<std::size_t>(g)])
                    ++cnt[key_of(j)];
                for (std::int64_t j : buckets[static_cast<std::size_t>(g)])
                    counts[static_cast<std::size_t>(j)] = cnt[key_of(j)];
            }
        });
    return counts;
}

/// Composite row-key convenience: `keys[j]` is already materialized.
inline std::vector<std::uint8_t> radix_first_seen_mask(
    const std::vector<std::string>& keys, std::int64_t n) {
    return radix_first_seen_by<std::string>(
        n, [&](std::int64_t j) { return keys[static_cast<std::size_t>(j)]; });
}

/// Composite row-key convenience: `keys[j]` is already materialized.
inline std::vector<std::int64_t> radix_key_counts(
    const std::vector<std::string>& keys, std::int64_t n) {
    return radix_counts_by<std::string>(
        n, [&](std::int64_t j) { return keys[static_cast<std::size_t>(j)]; });
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_RADIX_DEDUP_H
