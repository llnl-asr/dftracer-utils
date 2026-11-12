// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/store/page_cache.h>

#include <utility>

namespace dftracer::utils::store::engine {

PageCache::PageCache(std::size_t capacity) : share_(capacity / SHARDS) {}

std::uint64_t PageCache::KeyHash::operator()(const Key& key) const {
    return dstd::hash::splitmix64(key.offset ^
                                  dstd::hash::splitmix64(key.file));
}

PageCache::Shard& PageCache::shard_of(const Key& key) {
    return shards_[KeyHash{}(key) % SHARDS];
}

CachedPage PageCache::find(std::uint64_t file, std::uint64_t offset) {
    const Key key{file, offset};
    Shard& shard = shard_of(key);
    std::lock_guard<std::mutex> locked(shard.mutex);
    const auto found = shard.index.find(key);
    if (found == shard.index.end()) return CachedPage{};
    Slot& slot = shard.slots[found->second];
    slot.referenced = true;
    return slot.page;
}

CachedPage PageCache::insert(std::uint64_t file, std::uint64_t offset,
                             CachedPage page) {
    const Key key{file, offset};
    Shard& shard = shard_of(key);
    std::lock_guard<std::mutex> locked(shard.mutex);
    const auto found = shard.index.find(key);
    if (found != shard.index.end()) {
        Slot& slot = shard.slots[found->second];
        slot.referenced = true;
        return slot.page;
    }
    std::size_t at = shard.slots.size();
    if (!shard.free.empty()) {
        at = shard.free.back();
        shard.free.pop_back();
    } else {
        shard.slots.emplace_back();
    }
    Slot& slot = shard.slots[at];
    slot.page = std::move(page);
    slot.key = key;
    slot.bytes = slot.page->size();
    slot.referenced = false;
    shard.index.emplace(key, at);
    shard.bytes += slot.bytes;
    CachedPage kept = slot.page;

    while (shard.bytes > share_ && shard.index.size() > 1) {
        if (shard.hand >= shard.slots.size()) shard.hand = 0;
        Slot& victim = shard.slots[shard.hand];
        if (shard.hand == at) {
            // The page just kept stays: a page read once is the next to go.
        } else if (victim.page != nullptr && victim.referenced) {
            victim.referenced = false;
        } else if (victim.page != nullptr) {
            shard.index.erase(victim.key);
            shard.bytes -= victim.bytes;
            victim.page.reset();
            victim.bytes = 0;
            shard.free.push_back(shard.hand);
        }
        ++shard.hand;
    }
    return kept;
}

std::size_t PageCache::bytes() const {
    std::size_t total = 0;
    for (const Shard& shard : shards_) {
        std::lock_guard<std::mutex> locked(shard.mutex);
        total += shard.bytes;
    }
    return total;
}

}  // namespace dftracer::utils::store::engine
