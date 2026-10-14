#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_HASH_ARENA_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_HASH_ARENA_H

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace dftracer::utils::dataframe::lazy_internal {

/// Exact first-seen state over keys the caller hashes, with no per-key heap
/// allocation. The bytes of each distinct key live once in a growing arena and
/// a hash maps to the chain of keys that share it, so two keys with the same
/// hash still compare their real bytes. One entry per distinct key, never one
/// per input row: a duplicate pays only a lookup and a compare. Not
/// thread-safe; `hash` must be a pure function of the key bytes.
class HashArena {
   public:
    /// True when `key` was not present (and is now); false on a duplicate.
    bool insert(std::uint64_t hash, const char* key, std::uint32_t len) {
        const auto it = head_.find(hash);
        std::uint32_t next = 0;
        if (it != head_.end()) {
            next = it->second;
            if (find_in_chain(next, key, len)) return false;
        }
        Node fresh;
        fresh.off = bytes_.size();
        fresh.len = len;
        fresh.next = next;
        bytes_.insert(bytes_.end(), key, key + len);
        nodes_.push_back(fresh);
        const auto node = static_cast<std::uint32_t>(nodes_.size());
        if (it != head_.end())
            it->second = node;
        else
            head_.emplace(hash, node);
        return true;
    }

    bool contains(std::uint64_t hash, const char* key,
                  std::uint32_t len) const {
        const auto it = head_.find(hash);
        return it != head_.end() && find_in_chain(it->second, key, len);
    }

   private:
    struct Node {
        std::uint64_t off;
        std::uint32_t len;
        std::uint32_t next;
    };

    bool find_in_chain(std::uint32_t node, const char* key,
                       std::uint32_t len) const {
        while (node != 0) {
            const Node& n = nodes_[node - 1];
            if (n.len == len &&
                (len == 0 || std::memcmp(bytes_.data() + n.off, key, len) == 0))
                return true;
            node = n.next;
        }
        return false;
    }

    std::vector<char> bytes_;
    std::vector<Node> nodes_;
    ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> head_;
};

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_HASH_ARENA_H
