// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_CACHE_CACHE_STORE_H
#define DFTRACER_UTILS_INDEX_CACHE_CACHE_STORE_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/store/store.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::cache {

/// Usage record value: bytes u64, last use u64 (microseconds since the epoch),
/// both big-endian.
inline constexpr std::size_t USAGE_BYTES = 16;

/// One cache folder on the store: entries keyed by a 64-bit signature, each
/// one copy of its own scope that is written whole, plus a usage record per
/// entry for least recently used eviction. Every process may write the same
/// folder. Errors throw DFTUtilsException(IO).
class CacheStore {
   public:
    enum class Mode : std::uint8_t { ReadOnly, ReadWrite };

    struct Usage {
        std::uint64_t sig = 0;
        std::uint64_t bytes = 0;
        std::uint64_t last_used = 0;
    };

    /// Kept open for the life of the process. ReadWrite creates the folder.
    /// Null when the store cannot be opened or, for ReadOnly, the folder holds
    /// none. Never throws.
    static dstd::SharedPtr<CacheStore> open(const std::string& path, Mode mode);

    static bool exists(const std::string& path);

    bool writable() const noexcept { return writable_; }

    dstd::Optional<std::string> get(std::uint64_t sig,
                                    dstd::StringView key) const;
    /// In key order.
    void scan(std::uint64_t sig, dstd::StringView prefix,
              const std::function<void(dstd::StringView, dstd::StringView)>& fn)
        const;
    /// Entry by entry in signature order. `fn` returns false to stop.
    void scan_all(dstd::StringView begin, dstd::StringView end,
                  const std::function<bool(std::uint64_t, dstd::StringView,
                                           dstd::StringView)>& fn) const;
    std::vector<Usage> usage() const;

    /// Visible whole to the next read. Then evicts the least recently used
    /// entries until the usage total is at most cache_max_bytes(). `sig` goes
    /// only when it alone is over the budget. InvalidArgument when read-only.
    void put(std::uint64_t sig,
             const std::vector<dstd::Pair<std::string, std::string>>& entries,
             std::uint64_t bytes);
    /// Marks `sig` used now. No-op when read-only or `sig` has no record.
    void touch(std::uint64_t sig);

    CacheStore(dftracer::utils::store::Store store,
               dftracer::utils::store::Namespace data,
               dftracer::utils::store::Namespace usage, bool writable);

   private:
    void evict(std::uint64_t keep);
    dftracer::utils::store::Snapshot snapshot() const;

    mutable dftracer::utils::store::Store store_;
    dftracer::utils::store::Namespace data_;
    dftracer::utils::store::Namespace usage_;
    bool writable_ = false;
};

/// The size each cache store keeps at most: DFTRACER_UTILS_CACHE_MAX_BYTES
/// (bytes, or a size such as "512MB"), 2 GiB when unset or malformed.
std::uint64_t cache_max_bytes();

/// Closes every cache folder this process kept open.
void clear_cache_roots();

}  // namespace dftracer::utils::index::cache

#endif  // DFTRACER_UTILS_INDEX_CACHE_CACHE_STORE_H
