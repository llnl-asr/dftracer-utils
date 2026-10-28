#ifndef DFTRACER_UTILS_INDEX_CACHE_CACHE_STORE_H
#define DFTRACER_UTILS_INDEX_CACHE_CACHE_STORE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {
class StoreRoot;
}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::cache {

/// Size of a usage record value: bytes u64, last use u64 (microseconds since
/// the epoch), both big-endian.
inline constexpr std::size_t USAGE_BYTES = 16;

/// One cache folder on the store: entries keyed by a 64-bit signature, each
/// one copy of its own scope that is written whole, plus a usage record per
/// entry for least recently used eviction. Every process may write the same
/// folder; each writes its own data file. Errors throw DFTUtilsException(IO).
class CacheStore {
   public:
    enum class Mode : std::uint8_t { ReadOnly, ReadWrite };

    struct Usage {
        std::uint64_t sig = 0;
        std::uint64_t bytes = 0;
        std::uint64_t last_used = 0;
    };

    /// The store of `path`, kept open for the life of the process. ReadWrite
    /// creates the folder; ReadOnly yields null when the folder holds no store.
    /// Null when the store cannot be opened; never throws.
    static std::shared_ptr<CacheStore> open(const std::string& path, Mode mode);

    /// True when `path` holds a store a ReadOnly open finds.
    static bool exists(const std::string& path);

    bool writable() const noexcept { return writable_; }

    /// The value of `key` in entry `sig`.
    std::optional<std::string> get(std::uint64_t sig,
                                   std::string_view key) const;
    /// Calls `fn` with each key of entry `sig` that starts with `prefix`, and
    /// its value, in key order.
    void scan(std::uint64_t sig, std::string_view prefix,
              const std::function<void(std::string_view, std::string_view)>& fn)
        const;
    /// Calls `fn` with each key in [begin, end) of every entry, entry by entry
    /// in signature order; `fn` returns false to stop.
    void scan_all(std::string_view begin, std::string_view end,
                  const std::function<bool(std::uint64_t, std::string_view,
                                           std::string_view)>& fn) const;
    /// The usage record of every entry.
    std::vector<Usage> usage() const;

    /// Writes `entries` as entry `sig` with a usage record of `bytes`, visible
    /// whole to the next read, then evicts the least recently used entries
    /// until the usage records total at most cache_max_bytes(); `sig` goes
    /// only when it alone is over the budget. InvalidArgument when read-only.
    void put(std::uint64_t sig,
             const std::vector<std::pair<std::string, std::string>>& entries,
             std::uint64_t bytes);
    /// Marks `sig` used now. No-op when read-only or `sig` has no record.
    void touch(std::uint64_t sig);

    CacheStore(std::shared_ptr<store::StoreRoot> root, bool writable);

   private:
    void evict(std::uint64_t keep);

    std::shared_ptr<store::StoreRoot> root_;
    bool writable_ = false;
};

/// The size each cache store keeps at most: DFTRACER_UTILS_CACHE_MAX_BYTES
/// (bytes, or a size such as "512MB"), 2 GiB when unset or malformed.
std::uint64_t cache_max_bytes();

}  // namespace dftracer::utils::index::cache

#endif  // DFTRACER_UTILS_INDEX_CACHE_CACHE_STORE_H
