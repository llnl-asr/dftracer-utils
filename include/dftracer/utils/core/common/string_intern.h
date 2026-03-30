#ifndef DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
#define DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H

#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils {

/**
 * @brief Thread-safe string interning table.
 *
 * Stores each unique string once and returns an integer ID.
 * Lookups by string_view avoid allocation on cache hit.
 * IDs are stable for the lifetime of the table.
 *
 * Usage:
 * @code
 *   StringIntern intern;
 *   uint32_t id = intern.get_or_insert("POSIX");  // first call: stores string
 *   uint32_t id2 = intern.get_or_insert("POSIX"); // cache hit: no alloc
 *   assert(id == id2);
 *   assert(intern.resolve(id) == "POSIX");
 * @endcode
 */
class StringIntern {
   public:
    StringIntern() = default;

    // Non-copyable, non-movable (shared_mutex is not movable)
    StringIntern(const StringIntern&) = delete;
    StringIntern& operator=(const StringIntern&) = delete;
    StringIntern(StringIntern&&) = delete;
    StringIntern& operator=(StringIntern&&) = delete;

    /**
     * @brief Intern a string. Returns its unique ID.
     * Thread-safe. Uses shared_mutex: concurrent reads, exclusive writes.
     * Lookups use string_view (no allocation on cache hit).
     */
    std::uint32_t get_or_insert(std::string_view sv) {
        // Fast path: read lock, check if already interned
        {
            std::shared_lock lock(mutex_);
            auto it = str_to_id_.find(sv);
            if (it != str_to_id_.end()) return it->second;
        }
        // Slow path: write lock, insert new string
        std::unique_lock lock(mutex_);
        // Double-check after acquiring write lock
        auto [it, inserted] = str_to_id_.try_emplace(
            std::string(sv), static_cast<std::uint32_t>(id_to_str_.size()));
        if (inserted) {
            id_to_str_.push_back(it->first);
        }
        return it->second;
    }

    /**
     * @brief Resolve an ID back to its string. Thread-safe.
     */
    std::string_view resolve(std::uint32_t id) const {
        std::shared_lock lock(mutex_);
        return id_to_str_[id];
    }

    /**
     * @brief Intern a string and return a stable string_view.
     * Convenience wrapper: inserts if new, then resolves to string_view.
     */
    std::string_view intern(std::string_view sv) {
        return resolve(get_or_insert(sv));
    }

    /**
     * @brief Number of unique strings interned.
     */
    std::size_t size() const {
        std::shared_lock lock(mutex_);
        return id_to_str_.size();
    }

   private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::uint32_t, TransparentStringHash,
                       TransparentStringEqual>
        str_to_id_;
    // Stores references to the map's owned strings for O(1) resolve.
    std::vector<std::string_view> id_to_str_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
