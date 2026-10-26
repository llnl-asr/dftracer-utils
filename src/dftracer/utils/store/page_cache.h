#ifndef DFTRACER_UTILS_STORE_PAGE_CACHE_H
#define DFTRACER_UTILS_STORE_PAGE_CACHE_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::store {

/// The raw bytes of one decoded page, shared by the cache and every query that
/// reads it: a page the cache drops lives until the last query lets go of it.
using CachedPage = dstd::SharedPtr<const dstd::Vector<std::uint8_t>>;

/// Decoded pages by the file they come from and the offset of their stored
/// bytes in it, at most about `capacity` bytes of them, dropped by CLOCK: a
/// page found since the hand last passed it gets a second chance, and a page
/// kept but not found again goes first. Thread-safe: a key hashes to one of
/// SHARDS shards, each with its own mutex and an equal share of the capacity.
class PageCache {
   public:
    explicit PageCache(std::size_t capacity);
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    /// The page at `offset` of file `file`, or null when the cache has none.
    CachedPage find(std::uint64_t file, std::uint64_t offset);
    /// Keeps `page` as the page at `offset` of file `file`, unless the cache
    /// holds one there already, and returns the page it holds. Drops pages of
    /// the shard until the shard is within its share, never the last one.
    CachedPage insert(std::uint64_t file, std::uint64_t offset,
                      CachedPage page);
    /// The bytes of the pages the cache holds.
    std::size_t bytes() const;

   private:
    static constexpr std::size_t SHARDS = 16;

    struct Key {
        std::uint64_t file = 0;
        std::uint64_t offset = 0;
        bool operator==(const Key& other) const {
            return file == other.file && offset == other.offset;
        }
    };
    struct KeyHash {
        using is_avalanching = void;
        std::uint64_t operator()(const Key& key) const;
    };
    struct Slot {
        CachedPage page;
        Key key;
        std::size_t bytes = 0;
        bool referenced = false;
    };
    struct Shard {
        mutable std::mutex mutex;
        dstd::HashMap<Key, std::size_t, KeyHash> index;
        dstd::Vector<Slot> slots;
        dstd::Vector<std::size_t> free;
        std::size_t hand = 0;
        std::size_t bytes = 0;
    };

    Shard& shard_of(const Key& key);

    std::size_t share_ = 0;
    Shard shards_[SHARDS];
};

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_PAGE_CACHE_H
