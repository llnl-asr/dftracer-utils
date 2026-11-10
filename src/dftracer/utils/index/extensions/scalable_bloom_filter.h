// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_SCALABLE_BLOOM_FILTER_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_SCALABLE_BLOOM_FILTER_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/bloom_filter.h>

#include <cstddef>

namespace dftracer::utils::index::extensions {

/**
 * @brief Cardinality-adaptive Bloom filter over a chain of BloomFilters.
 *
 * A plain @ref BloomFilter fixes its bit count at construction, so a chunk
 * holding far more distinct values than the configured expectation
 * saturates and its false-positive rate approaches 1 - it stops pruning
 * anything. An indexer cannot know a chunk's distinct-value count before
 * streaming it, so this grows instead: values land in the newest level,
 * and when that level reaches its capacity a larger one is opened. A probe
 * ORs the levels, so a miss in all of them is still a definite miss.
 *
 * Levels grow geometrically and their false-positive targets tighten by
 * the same ratio, which keeps the compounded rate of the chain bounded
 * (Almeida et al., "Scalable Bloom Filters", Inf. Process. Lett. 101(6),
 * 2007).
 *
 * Levels are keyed by capacity, at most one per capacity class, so a
 * merge of two chains ORs matching levels rather than concatenating them.
 * Chain length therefore stays logarithmic in cardinality no matter how
 * many parallel slices are merged together.
 *
 * Serialization format:
 *   ["SBF1" magic (4 bytes)]
 *   [4 bytes: level count (uint32_t LE)]
 *   [8 bytes: initial capacity (uint64_t LE)]
 *   [8 bytes: false positive rate (IEEE double, LE)]
 *   [per level: 8 bytes capacity (uint64_t LE), 4 bytes blob length
 * (uint32_t LE), then a @ref BloomFilter blob]
 *
 * A loaded chain takes more values and merges as the one it was saved from.
 * @ref from_blob returns a Parse error on any other bytes.
 */
class ScalableBloomFilter {
   public:
    static constexpr std::size_t GROWTH_FACTOR = 4;
    static constexpr double TIGHTENING_RATIO = 0.5;

    explicit ScalableBloomFilter(std::size_t initial_capacity = 1024,
                                 double false_positive_rate = 0.01);

    /// A Parse error when the bytes are not a saved chain.
    static dstd::Result<ScalableBloomFilter> from_blob(
        const unsigned char* data, std::size_t size);

    void add(dstd::StringView value);
    bool possibly_contains(dstd::StringView value) const;
    /// One per add() call, whatever the chain's length.
    std::size_t hash_computations() const { return hashed_; }
    void merge_from(const ScalableBloomFilter& other);

    dstd::Vector<unsigned char> serialize() const;
    void serialize_into(dstd::Vector<unsigned char>& result) const;

    /// Approximate count of distinct values added. Exact up to the
    /// false-positive rate of the levels, since `add` skips values that
    /// already probe as present.
    std::size_t entry_count() const { return distinct_; }
    std::size_t level_count() const { return levels_.size(); }
    std::size_t size_bytes() const;

   private:
    struct Level {
        BloomFilter filter;
        std::size_t capacity;
        std::size_t distinct;
    };

    void open_level(std::size_t capacity);

    dstd::Vector<Level> levels_;
    std::size_t initial_capacity_;
    double false_positive_rate_;
    std::size_t distinct_ = 0;
    std::size_t hashed_ = 0;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_SCALABLE_BLOOM_FILTER_H
