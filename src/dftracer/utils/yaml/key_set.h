// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_KEY_SET_H
#define DFTRACER_UTILS_YAML_KEY_SET_H

#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::yaml {

/// The most map levels a block or flow nesting holds.
constexpr std::size_t MAX_DEPTH = 128;

/// A 32-bit hash of a key from its first and last 8 bytes and its length,
/// which are the whole key up to 16 bytes. `limit` ends the text: a load of 8
/// bytes stays inside it.
inline std::uint32_t hash_key(const char* p, std::size_t n, const char* limit) {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (n >= 8) {
        std::memcpy(&first, p, 8);
        std::memcpy(&last, p + n - 8, 8);
    } else if (n != 0) {
        if (p + 8 <= limit) {
            std::memcpy(&first, p, 8);
            first &= (std::uint64_t{1} << (8 * n)) - 1;
        } else {
            std::memcpy(&first, p, n);
        }
        last = first;
    }
    std::uint64_t h = first * 0x9E3779B97F4A7C15ull;
    h ^= (last + n) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 32;
    return static_cast<std::uint32_t>(h);
}

/// The keys of the open maps of one reader, so that a repeated key is found as
/// it is added. A map with fewer keys than LINEAR_KEY_LIMIT keeps a 32-bit
/// hash for each key and a set of bits over them, and looks at the earlier keys
/// only when the bit of a new key is set; a map with more uses a hash table.
class KeySet {
   public:
    /// The state of one open map. `slot` of a call is the depth of the map,
    /// below MAX_DEPTH.
    struct Level {
        std::uint64_t bloom[4];
        std::uint32_t base;
        std::uint32_t count;
        bool has_table;
    };

    void open(Level& level) {
        level.bloom[0] = level.bloom[1] = level.bloom[2] = level.bloom[3] = 0;
        level.base = static_cast<std::uint32_t>(keys_.size());
        level.count = 0;
        level.has_table = false;
    }

    void close(Level& level, std::size_t slot) {
        keys_.resize(level.base);
        if (level.has_table) tables_[slot].clear();
    }

    /// Adds the key `key` of the entry `id`. False when an earlier key of the
    /// map is the same: `same(other_id, key)` tells it. `limit` ends the text
    /// that holds the key, or is the start of the key when it is somewhere
    /// else.
    template <class Same>
    bool add(Level& level, std::size_t slot, std::uint32_t id,
             dstd::StringView key, const char* limit, Same&& same) {
        const std::uint32_t hash = hash_key(key.data(), key.size(), limit);
        const std::uint32_t index = level.count;
        if (index >= LINEAR_KEY_LIMIT) {
            if (!table_insert(level, slot, hash, id, key, same)) return false;
        } else {
            std::uint64_t& word = level.bloom[(hash >> 6) & 3];
            const std::uint64_t bit = std::uint64_t{1} << (hash & 63);
            if ((word & bit) != 0) {
                for (std::uint32_t i = 0; i < index; ++i) {
                    const std::uint64_t entry = keys_[level.base + i];
                    if (static_cast<std::uint32_t>(entry >> 32) == hash &&
                        same(static_cast<std::uint32_t>(entry), key))
                        return false;
                }
            }
            word |= bit;
        }
        ++level.count;
        keys_.push_back((static_cast<std::uint64_t>(hash) << 32) | id);
        return true;
    }

    void clear() {
        keys_.clear();
        for (auto& table : tables_) table.clear();
    }

   private:
    static constexpr std::size_t LINEAR_KEY_LIMIT = 64;

    template <class Same>
    bool table_insert(Level& level, std::size_t slot_of_level,
                      std::uint32_t hash, std::uint32_t id,
                      dstd::StringView key, Same&& same) {
        dstd::Vector<std::uint64_t>& table = tables_[slot_of_level];
        const std::size_t want =
            (static_cast<std::size_t>(level.count) + 1) * 2;
        if (table.size() < want) {
            std::size_t n = std::max<std::size_t>(table.size() * 2, 256);
            while (n < want) n *= 2;
            table.assign(n, 0);
            const std::size_t mask = n - 1;
            for (std::size_t i = level.base; i < keys_.size(); ++i) {
                const auto entry_hash =
                    static_cast<std::uint32_t>(keys_[i] >> 32);
                std::size_t slot = entry_hash & mask;
                while (table[slot] != 0) slot = (slot + 1) & mask;
                table[slot] = (static_cast<std::uint64_t>(entry_hash) << 32) |
                              ((keys_[i] & 0xFFFFFFFFull) + 1ull);
            }
            level.has_table = true;
        }
        const std::size_t mask = table.size() - 1;
        std::size_t slot = hash & mask;
        while (table[slot] != 0) {
            if ((table[slot] >> 32) == hash &&
                same(static_cast<std::uint32_t>(table[slot]) - 1, key))
                return false;
            slot = (slot + 1) & mask;
        }
        table[slot] = (static_cast<std::uint64_t>(hash) << 32) | (id + 1ull);
        return true;
    }

    /// The hash, then the id, of each key of the open maps.
    dstd::RawBuffer<std::uint64_t> keys_;
    dstd::Vector<std::uint64_t> tables_[MAX_DEPTH];
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_KEY_SET_H
