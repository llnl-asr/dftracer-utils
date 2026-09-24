#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_SCALABLE_BLOOM_FILTER_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_SCALABLE_BLOOM_FILTER_H

#include <dftracer/utils/index/extensions/bloom_filter.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::extensions {

/**
 * @brief Cardinality-adaptive Bloom filter over a chain of BloomFilters.
 *
 * A plain BloomFilter fixes its bit count at construction, so a chunk
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
 *   [per level: 4 bytes blob length (uint32_t LE), then a BloomFilter blob]
 *
 * `from_blob` accepts a bare BloomFilter blob too, reading it as a
 * single-level chain, so blooms written before this type existed still
 * load.
 */
class ScalableBloomFilter {
   public:
    static constexpr std::size_t GROWTH_FACTOR = 4;
    static constexpr double TIGHTENING_RATIO = 0.5;

    explicit ScalableBloomFilter(std::size_t initial_capacity = 1024,
                                 double false_positive_rate = 0.01);

    static ScalableBloomFilter from_blob(const unsigned char* data,
                                         std::size_t size);

    void add(std::string_view value);
    bool possibly_contains(std::string_view value) const;

    /// Union `other` into this filter. Levels of equal geometry are OR'd;
    /// levels with no counterpart are adopted as-is.
    void merge_from(const ScalableBloomFilter& other);

    std::vector<unsigned char> serialize() const;
    void serialize_into(std::vector<unsigned char>& result) const;

    /// Approximate count of distinct values added. Exact up to the
    /// false-positive rate of the levels, since `add` skips values that
    /// already probe as present.
    std::size_t num_entries() const { return distinct_; }
    std::size_t num_levels() const { return levels_.size(); }
    std::size_t size_bytes() const;

   private:
    struct Level {
        BloomFilter filter;
        std::size_t capacity;
        std::size_t distinct;
    };

    void open_level(std::size_t capacity);

    std::vector<Level> levels_;
    std::size_t initial_capacity_;
    double false_positive_rate_;
    std::size_t distinct_ = 0;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_SCALABLE_BLOOM_FILTER_H
