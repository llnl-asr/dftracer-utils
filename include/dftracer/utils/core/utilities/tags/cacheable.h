#ifndef DFTRACER_UTILS_CORE_UTILITIES_TAGS_CACHEABLE_H
#define DFTRACER_UTILS_CORE_UTILITIES_TAGS_CACHEABLE_H

#include <chrono>
#include <cstddef>

namespace dftracer::utils::utilities::tags {

/**
 * @brief Configuration tag indicating that utility results can be cached.
 *
 * This tag enables automatic caching behavior when used with the behavior
 * system. Contains configuration parameters for cache management.
 */
struct Cacheable {
    /** Maximum number of entries to store in cache */
    std::size_t max_cache_size = 1000;

    /** Time-to-live for cached entries */
    std::chrono::seconds ttl{3600};

    /** Whether to use LRU (Least Recently Used) eviction policy */
    bool use_lru = true;
};

}  // namespace dftracer::utils::utilities::tags

#endif  // DFTRACER_UTILS_CORE_UTILITIES_TAGS_CACHEABLE_H
