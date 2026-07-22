#ifndef DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
#define DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H

#include <dftracer/utils/core/common/error.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace dftracer::utils {

class StringIntern {
    using Slot = std::atomic<const std::string*>;

   public:
    // id -> string lives in lazily allocated blocks behind a fixed directory,
    // so the table costs a block per 4096 ids actually used instead of one
    // preallocated array, and no id is unaddressable.
    static constexpr std::size_t BLOCK_BITS = 12;
    static constexpr std::size_t BLOCK_SIZE = 1u << BLOCK_BITS;
    static constexpr std::size_t DIRECTORY_SIZE = 1u << 16;
    static constexpr std::size_t FAST_CAPACITY = BLOCK_SIZE * DIRECTORY_SIZE;

    StringIntern()
        : buckets_(std::make_unique<std::atomic<Node*>[]>(BUCKET_COUNT)),
          directory_(std::make_unique<std::atomic<Slot*>[]>(DIRECTORY_SIZE)) {
        for (std::size_t i = 0; i < BUCKET_COUNT; ++i) {
            buckets_[i].store(nullptr, std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < DIRECTORY_SIZE; ++i) {
            directory_[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    ~StringIntern() {
        for (std::size_t i = 0; i < DIRECTORY_SIZE; ++i) {
            delete[] directory_[i].load(std::memory_order_relaxed);
        }
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
            // Content-derived so the same string gets the same id in every
            // process. A collision is refused below rather than silently
            // binding two strings to one id.
            id = static_cast<std::uint32_t>(h & (FAST_CAPACITY - 1));
        } else {
            id = static_cast<std::uint32_t>(
                num_strings_.load(std::memory_order_relaxed));
        }
        auto* new_node = new Node{std::string(sv), h, id, {}};
        new_node->next.store(buckets_[bucket].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);

        // Two strings landing on one deterministic id would merge unrelated
        // aggregation keys, so refuse rather than let the first one win.
        if (const auto* existing = slot_value(id)) {
            if (*existing != sv) {
                delete new_node;
                throw DFTUtilsException(
                    ErrorCode::INTERNAL,
                    "string intern: deterministic id collision between '" +
                        *existing + "' and '" + std::string(sv) +
                        "'; the dictionary is too large for this id space");
            }
        } else {
            bind_slot(id, &new_node->str);
        }

        // Publishing the bucket releases the slot store above too.
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

    /// Insert or look up a string at a specific id, for loading a persisted
    /// dictionary where ids must be preserved. Ids are index-local, so loading
    /// a second index's dictionary here would rebind them: a conflicting id
    /// throws rather than resolve to the wrong string.
    /// Safe to call concurrently with other inserts; must be called before
    /// any `resolve(id)` at that id.
    void insert_at_id(std::uint32_t id, std::string_view sv) {
        const auto h = hash(sv);
        const auto bucket = h & BUCKET_MASK;

        std::lock_guard lock(insert_mutex_);

        if (const auto* existing = slot_value(id)) {
            if (*existing == sv) return;
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "string intern: id " + std::to_string(id) + " is already '" +
                    *existing + "', cannot rebind to '" + std::string(sv) +
                    "' (dictionaries from two indexes in one table?)");
        }

        // Also avoid inserting a second node for the same string (would leave
        // the older node referenced by the bucket chain pointing at a stale
        // id, confusing get_or_insert which returns the first match).
        auto* node = buckets_[bucket].load(std::memory_order_acquire);
        while (node) {
            if (node->hash == h && node->str == sv) {
                // Same string under another id: bind this id to it too.
                bind_slot(id, &node->str);
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

        bind_slot(id, &new_node->str);

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

    /// Empty means "no string at this id". An id past the table's reach is a
    /// corrupt or foreign key, not an absent string, and says so.
    std::string_view resolve(std::uint32_t id) const {
        if (id >= FAST_CAPACITY) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "string intern: id " + std::to_string(id) + " is out of range");
        }
        auto* block =
            directory_[id >> BLOCK_BITS].load(std::memory_order_acquire);
        if (!block) return {};
        auto* p = block[id & (BLOCK_SIZE - 1)].load(std::memory_order_acquire);
        return p ? std::string_view(*p) : std::string_view{};
    }

    /// The string bound to `id`, or null when the slot is free. Callers under
    /// `insert_mutex_` use this to spot a conflicting binding.
    const std::string* slot_value(std::uint32_t id) const {
        if (id >= FAST_CAPACITY) return nullptr;
        auto* block =
            directory_[id >> BLOCK_BITS].load(std::memory_order_acquire);
        if (!block) return nullptr;
        return block[id & (BLOCK_SIZE - 1)].load(std::memory_order_acquire);
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

    /// Derive ids from string content instead of a counter, so the same
    /// string gets the same id in every process. Needed where keys embedding
    /// string ids must match across MPI ranks for RocksDB merge operators to
    /// combine them.
    ///
    /// The id space is finite, so this only holds for dictionaries small
    /// enough to avoid a birthday collision; a collision throws rather than
    /// bind two strings to one id. Dictionaries that scale with the trace
    /// (per-file hashes) need sequential ids and a per-index table.
    ///
    /// Must be called before any `get_or_insert`.
    void enable_deterministic_ids() noexcept {
        deterministic_ids_.store(true, std::memory_order_release);
    }

   private:
    /// Bind `id` to `str`. Allocates the block on first use. Caller holds
    /// `insert_mutex_`.
    void bind_slot(std::uint32_t id, const std::string* str) {
        if (id >= FAST_CAPACITY) {
            throw DFTUtilsException(
                ErrorCode::INTERNAL,
                "string intern: exhausted the id space at " +
                    std::to_string(id));
        }
        auto& dir = directory_[id >> BLOCK_BITS];
        auto* block = dir.load(std::memory_order_acquire);
        if (!block) {
            block = new Slot[BLOCK_SIZE];
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i)
                block[i].store(nullptr, std::memory_order_relaxed);
            dir.store(block, std::memory_order_release);
        }
        block[id & (BLOCK_SIZE - 1)].store(str, std::memory_order_release);
    }

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
    std::unique_ptr<std::atomic<Slot*>[]> directory_;
    std::atomic<std::size_t> num_strings_{0};
    std::atomic<bool> deterministic_ids_{false};
    std::mutex insert_mutex_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
