// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_KEY_SET_H
#define DFTRACER_UTILS_YAML_KEY_SET_H

#include <dftracer/utils/common/export.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/hash/constants.h>
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

/// A key of under 8 bytes as one word: its bytes, then its length in the top
/// byte. Two such keys are the same when their words are. `limit` ends the
/// text: a load of 8 bytes stays inside it.
inline std::uint64_t short_key_word(const char* p, std::size_t n,
                                    const char* limit) {
    std::uint64_t word = 0;
    if (p + 8 <= limit) {
        std::memcpy(&word, p, 8);
        word &= (std::uint64_t{1} << (8 * n)) - 1;
    } else {
        std::memcpy(&word, p, n);
    }
    return word | (static_cast<std::uint64_t>(n) << 56);
}

inline std::uint32_t hash_short_word(std::uint64_t word) {
    return static_cast<std::uint32_t>((word * dstd::hash::GOLDEN_RATIO) >> 32);
}

/// A 32-bit hash of a key from its first and last 8 bytes and its length,
/// which are the whole key up to 16 bytes. `limit` is as for short_key_word.
inline std::uint32_t hash_key(const char* p, std::size_t n, const char* limit) {
    if (n < 8) return hash_short_word(short_key_word(p, n, limit));
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::memcpy(&first, p, 8);
    std::memcpy(&last, p + n - 8, 8);
    std::uint64_t h = first * dstd::hash::GOLDEN_RATIO;
    h ^= (last + n) * dstd::hash::XXH64_PRIME2;
    h ^= h >> 32;
    return static_cast<std::uint32_t>(h);
}

/// The keys of the open maps of one reader, so that a repeated key is found as
/// it is added. A map keeps its first SMALL_KEYS keys of under 8 bytes as
/// words, each with a one-byte tag, and compares a new one with the words whose
/// tag is the same. Past that, or at the first
/// longer key, it keeps a 32-bit hash for each key and a set of bits over them,
/// and looks at the earlier keys only when the bit of a new key is set; a map
/// with more than LINEAR_KEY_LIMIT keys uses a hash table.
class KeySet {
   public:
    static constexpr std::size_t SMALL_KEYS = 8;

    /// The state of one open map. `slot` of a call is the depth of the map,
    /// below MAX_DEPTH.
    struct Level {
        /// While `is_small`: the words of the first `count` keys.
        std::uint64_t small[SMALL_KEYS];
        /// While `is_small`: byte i is the tag of word i, 0 past `count`.
        std::uint64_t tags;
        std::uint64_t bloom[4];
        std::uint32_t small_ids[SMALL_KEYS];
        std::uint32_t base;
        std::uint32_t count;
        bool is_small;
        bool has_table;
    };

    void open(Level& level) {
        level.tags = 0;
        level.base = static_cast<std::uint32_t>(keys_.size());
        level.count = 0;
        level.is_small = true;
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
    DFTRACER_UTILS_ALWAYS_INLINE bool add(Level& level, std::size_t slot,
                                          std::uint32_t id,
                                          dstd::StringView key,
                                          const char* limit, Same&& same) {
        if (level.is_small && key.size() < 8 && level.count < SMALL_KEYS) {
            const std::uint64_t word =
                short_key_word(key.data(), key.size(), limit);
            // The bytes of `tags` equal to the tag of `word`, as the zero
            // bytes of their XOR, with a false hit only above a true one.
            const std::uint64_t tag = (word * dstd::hash::GOLDEN_RATIO) >> 56;
            const std::uint64_t x = level.tags ^ (tag * LOW_BYTES);
            std::uint64_t hits = (x - LOW_BYTES) & ~x & HIGH_BITS &
                                 ((std::uint64_t{1} << (8 * level.count)) - 1);
            while (hits != 0) [[unlikely]] {
                if (level.small[dstd::bits::ctz_u64(hits) >> 3] == word)
                    return false;
                hits &= hits - 1;
            }
            level.tags |= tag << (8 * level.count);
            level.small[level.count] = word;
            level.small_ids[level.count] = id;
            ++level.count;
            return true;
        }
        if (level.is_small) spill(level);
        const std::uint32_t hash = hash_key(key.data(), key.size(), limit);
        const std::uint32_t index = level.count;
        if (index >= LINEAR_KEY_LIMIT) {
            if (!table_insert(level, slot, hash, id, key, same)) return false;
        } else {
            std::uint64_t& word = level.bloom[(hash >> 6) & 3];
            const std::uint64_t bit = std::uint64_t{1} << (hash & 63);
            if ((word & bit) != 0 && has_key(level, hash, key, same))
                [[unlikely]]
                return false;
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

    static constexpr std::uint64_t LOW_BYTES = 0x0101010101010101ULL;
    static constexpr std::uint64_t HIGH_BITS = 0x8080808080808080ULL;

    // Out of line, as each is rare: the hot add stays small enough to inline.
    // Moves the words of a small map to the hashes and the set of bits.
    DFTRACER_UTILS_NOINLINE void spill(Level& level) {
        level.bloom[0] = level.bloom[1] = level.bloom[2] = level.bloom[3] = 0;
        for (std::uint32_t i = 0; i < level.count; ++i) {
            const std::uint32_t hash = hash_short_word(level.small[i]);
            level.bloom[(hash >> 6) & 3] |= std::uint64_t{1} << (hash & 63);
            keys_.push_back((static_cast<std::uint64_t>(hash) << 32) |
                            level.small_ids[i]);
        }
        level.is_small = false;
    }

    template <class Same>
    DFTRACER_UTILS_NOINLINE bool has_key(const Level& level, std::uint32_t hash,
                                         dstd::StringView key, Same& same) {
        for (std::uint32_t i = 0; i < level.count; ++i) {
            const std::uint64_t entry = keys_[level.base + i];
            if (static_cast<std::uint32_t>(entry >> 32) == hash &&
                same(static_cast<std::uint32_t>(entry), key))
                return true;
        }
        return false;
    }

    template <class Same>
    DFTRACER_UTILS_NOINLINE bool table_insert(
        Level& level, std::size_t slot_of_level, std::uint32_t hash,
        std::uint32_t id, dstd::StringView key, Same&& same) {
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
