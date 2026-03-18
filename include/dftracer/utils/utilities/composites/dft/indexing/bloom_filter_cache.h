#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_CACHE_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_CACHE_H

#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::indexing {

/// Thread-safe bounded cache for deserialized bloom filters.
/// Keyed by (idx_path, dimension, checkpoint_idx) for chunk blooms,
/// or (idx_path, dimension, UINT64_MAX) for file-level blooms.
/// When the cache exceeds max_entries, it is cleared entirely.
class BloomFilterCache {
   public:
    static constexpr std::size_t DEFAULT_MAX_ENTRIES = 10000;
    static constexpr std::uint64_t FILE_LEVEL_SENTINEL = UINT64_MAX;

    explicit BloomFilterCache(std::size_t max_entries = DEFAULT_MAX_ENTRIES)
        : max_entries_(max_entries) {}

    /// Look up a cached bloom filter. Returns nullopt on miss.
    std::optional<BloomFilter> get(const std::string& idx_path,
                                   const std::string& dimension,
                                   std::uint64_t checkpoint_idx) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(make_key(idx_path, dimension, checkpoint_idx));
        if (it == cache_.end()) return std::nullopt;
        return it->second;
    }

    /// Insert a bloom filter into the cache. Evicts all entries if full.
    void put(const std::string& idx_path, const std::string& dimension,
             std::uint64_t checkpoint_idx, const BloomFilter& bloom) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.size() >= max_entries_) {
            cache_.clear();
        }
        cache_.emplace(make_key(idx_path, dimension, checkpoint_idx), bloom);
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

   private:
    static std::string make_key(const std::string& idx_path,
                                const std::string& dimension,
                                std::uint64_t checkpoint_idx) {
        std::string key;
        key.reserve(idx_path.size() + dimension.size() + 24);
        key += idx_path;
        key += '\0';
        key += dimension;
        key += '\0';
        key += std::to_string(checkpoint_idx);
        return key;
    }

    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, BloomFilter> cache_;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_CACHE_H
