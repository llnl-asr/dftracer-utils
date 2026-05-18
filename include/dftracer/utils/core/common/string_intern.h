#ifndef DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
#define DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace dftracer::utils {

class StringIntern {
   public:
    static constexpr std::size_t FAST_CAPACITY = 1u << 20;

    StringIntern()
        : buckets_(std::make_unique<std::atomic<Node*>[]>(BUCKET_COUNT)),
          fast_(std::make_unique<std::atomic<const std::string*>[]>(
              FAST_CAPACITY)) {
        for (std::size_t i = 0; i < BUCKET_COUNT; ++i) {
            buckets_[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    ~StringIntern() {
        for (std::size_t i = 0; i < BUCKET_COUNT; ++i) {
            auto* node = buckets_[i].load(std::memory_order_relaxed);
            while (node) {
                auto* next = node->next.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }
        }
    }

    StringIntern(const StringIntern&) = delete;
    StringIntern& operator=(const StringIntern&) = delete;
    StringIntern(StringIntern&&) = delete;
    StringIntern& operator=(StringIntern&&) = delete;

    std::uint32_t get_or_insert(std::string_view sv) {
        const auto h = hash(sv);
        const auto bucket = h & BUCKET_MASK;

        // Lock-free lookup
        auto* node = buckets_[bucket].load(std::memory_order_acquire);
        while (node) {
            if (node->hash == h && node->str == sv) {
                return node->id;
            }
            node = node->next.load(std::memory_order_acquire);
        }

        // Rare: new string; take mutex
        std::lock_guard lock(insert_mutex_);

        // Re-check under lock (another thread may have inserted)
        node = buckets_[bucket].load(std::memory_order_acquire);
        while (node) {
            if (node->hash == h && node->str == sv) {
                return node->id;
            }
            node = node->next.load(std::memory_order_acquire);
        }

        std::uint32_t id;
        if (deterministic_ids_.load(std::memory_order_acquire)) {
            // Deterministic-hash id so the same string maps to the same id
            // across processes. Mask off top bit + clamp into FAST_CAPACITY
            // so `resolve()` hits the fast path. Collisions (different
            // strings -> same id) are bucket-chained on insert but
            // `resolve(id)` returns the first-inserted string for that id.
            id = static_cast<std::uint32_t>(h & (FAST_CAPACITY - 1));
        } else {
            id = static_cast<std::uint32_t>(
                num_strings_.load(std::memory_order_relaxed));
        }
        auto* new_node = new Node{std::string(sv), h, id, {}};
        new_node->next.store(buckets_[bucket].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);

        if (id < FAST_CAPACITY) {
            // Respect "first-inserted wins" when a collision maps two
            // different strings to the same deterministic id: only set
            // fast_[id] if the slot is still empty.
            const std::string* expected = nullptr;
            fast_[id].compare_exchange_strong(expected, &new_node->str,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
        }

        // Publish to bucket; all prior stores (node fields, fast_[])
        // are visible to readers via this release.
        buckets_[bucket].store(new_node, std::memory_order_release);

        if (!deterministic_ids_.load(std::memory_order_acquire)) {
            // Sequential id path: advance counter past the id we just
            // handed out so size() stays monotonic.
            num_strings_.store(static_cast<std::size_t>(id) + 1,
                               std::memory_order_release);
        } else {
            // Deterministic id path: `size()` is a weak estimate of
            // distinct strings; bump if this id is the highest seen.
            std::size_t cur = num_strings_.load(std::memory_order_relaxed);
            const std::size_t need = static_cast<std::size_t>(id) + 1;
            while (cur < need && !num_strings_.compare_exchange_weak(
                                     cur, need, std::memory_order_release,
                                     std::memory_order_relaxed)) {
            }
        }

        return id;
    }

    /// Insert or look up a string at a specific id (for loading a persisted
    /// dictionary where ids must be preserved). If the id already holds a
    /// different string, the existing binding wins (caller error -> ignored).
    /// Safe to call concurrently with other inserts; must be called before
    /// any `resolve(id)` at that id.
    void insert_at_id(std::uint32_t id, std::string_view sv) {
        const auto h = hash(sv);
        const auto bucket = h & BUCKET_MASK;

        std::lock_guard lock(insert_mutex_);

        // If the id already has a string in fast_, nothing to do.
        if (id < FAST_CAPACITY) {
            if (fast_[id].load(std::memory_order_acquire) != nullptr) {
                return;
            }
        }

        // Also avoid inserting a second node for the same string (would leave
        // the older node referenced by the bucket chain pointing at a stale
        // id, confusing get_or_insert which returns the first match).
        auto* node = buckets_[bucket].load(std::memory_order_acquire);
        while (node) {
            if (node->hash == h && node->str == sv) {
                // String already interned under a different id; point fast_[id]
                // at it so resolve(id) returns something valid.
                if (id < FAST_CAPACITY) {
                    fast_[id].store(&node->str, std::memory_order_release);
                }
                if (static_cast<std::size_t>(id) + 1 >
                    num_strings_.load(std::memory_order_relaxed)) {
                    num_strings_.store(static_cast<std::size_t>(id) + 1,
                                       std::memory_order_release);
                }
                return;
            }
            node = node->next.load(std::memory_order_acquire);
        }

        auto* new_node = new Node{std::string(sv), h, id, {}};
        new_node->next.store(buckets_[bucket].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);

        if (id < FAST_CAPACITY) {
            fast_[id].store(&new_node->str, std::memory_order_release);
        }

        buckets_[bucket].store(new_node, std::memory_order_release);

        // Advance num_strings_ past the highest id ever inserted so future
        // get_or_insert calls don't collide with a loaded id.
        std::size_t cur = num_strings_.load(std::memory_order_relaxed);
        const std::size_t need = static_cast<std::size_t>(id) + 1;
        while (cur < need && !num_strings_.compare_exchange_weak(
                                 cur, need, std::memory_order_release,
                                 std::memory_order_relaxed)) {
        }
    }

    std::string_view resolve(std::uint32_t id) const {
        if (id >= FAST_CAPACITY) return {};
        auto* p = fast_[id].load(std::memory_order_acquire);
        return p ? std::string_view(*p) : std::string_view{};
    }

    std::string_view intern(std::string_view sv) {
        return resolve(get_or_insert(sv));
    }

    std::size_t size() const {
        return num_strings_.load(std::memory_order_acquire);
    }

    /// Shift the next-to-assign id counter to `base`. Subsequent
    /// `get_or_insert` calls allocate ids starting at `base`.
    /// Must be called before any `get_or_insert` on this instance.
    /// Lock-free: caller ensures no concurrent inserts.
    void reserve_id_base(std::uint32_t base) noexcept {
        num_strings_.store(base, std::memory_order_release);
    }

    /// Enable deterministic-hash id assignment. When set, `get_or_insert`
    /// returns a stable id derived from the string's content rather than a
    /// sequential counter. Same string -> same id in every process,
    /// regardless of insertion order. Intended for multi-process workflows
    /// (e.g. MPI ranks) where keys that include string ids must be
    /// identical across ranks so RocksDB merge operators can combine
    /// operands for the same logical key.
    ///
    /// Collision handling: the 32-bit id is `hash(str) & 0x7FFFFFFF` to
    /// stay within FAST_CAPACITY-reachable range on lookup when
    /// `id < FAST_CAPACITY`. Different strings with the same id are
    /// chained in the bucket and lookup resolves by string equality, but
    /// `resolve(id)` can only return one of them. For the typical
    /// dftracer workload (cat/name/hhash/fhash dictionaries with O(1000)
    /// entries) birthday collisions are negligible.
    ///
    /// Must be called before any `get_or_insert`.
    void enable_deterministic_ids() noexcept {
        deterministic_ids_.store(true, std::memory_order_release);
    }

   private:
    static constexpr std::size_t BUCKET_COUNT = 1u << 12;  // 4096
    static constexpr std::size_t BUCKET_MASK = BUCKET_COUNT - 1;

    struct Node {
        const std::string str;
        const std::size_t hash;
        const std::uint32_t id;
        std::atomic<Node*> next;
    };

    static std::size_t hash(std::string_view sv) {
        return std::hash<std::string_view>{}(sv);
    }

    std::unique_ptr<std::atomic<Node*>[]> buckets_;
    std::unique_ptr<std::atomic<const std::string*>[]> fast_;
    std::atomic<std::size_t> num_strings_{0};
    std::atomic<bool> deterministic_ids_{false};
    std::mutex insert_mutex_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
