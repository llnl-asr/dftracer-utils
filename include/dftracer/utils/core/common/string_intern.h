#ifndef DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
#define DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils {

class StringIntern {
    using Slot = std::atomic<const std::string*>;
    using LogSlot = std::atomic<std::uint32_t>;
    using IndexSlot = std::atomic<std::uint32_t>;

   public:
    // id -> string lives in lazily allocated blocks behind a fixed directory,
    // so an idle table costs the directory rather than the whole id space.
    static constexpr std::size_t BLOCK_BITS = 12;
    static constexpr std::size_t BLOCK_SIZE = 1u << BLOCK_BITS;
    static constexpr std::size_t DIRECTORY_SIZE = 1u << 16;
    static constexpr std::size_t FAST_CAPACITY = BLOCK_SIZE * DIRECTORY_SIZE;

    static constexpr std::uint32_t NO_ID = UINT32_MAX;

    StringIntern()
        : directory_(std::make_unique<std::atomic<Slot*>[]>(DIRECTORY_SIZE)),
          log_(std::make_unique<std::atomic<LogSlot*>[]>(DIRECTORY_SIZE)) {
        for (std::size_t i = 0; i < DIRECTORY_SIZE; ++i) {
            directory_[i].store(nullptr, std::memory_order_relaxed);
            log_[i].store(nullptr, std::memory_order_relaxed);
        }
        index_.store(make_index(INITIAL_INDEX_SLOTS),
                     std::memory_order_release);
    }

    ~StringIntern() {
        // One index slot per distinct string, so this frees each exactly once
        // even where two ids share a string.
        auto* idx = index_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i <= idx->mask; ++i) {
            auto v = idx->slots[i].load(std::memory_order_relaxed);
            if (v != 0) delete const_cast<std::string*>(slot_value(v - 1));
        }
        delete_index(idx);
        for (auto* old : retired_) delete_index(old);
        for (std::size_t i = 0; i < DIRECTORY_SIZE; ++i) {
            delete[] directory_[i].load(std::memory_order_relaxed);
            delete[] log_[i].load(std::memory_order_relaxed);
        }
    }

    StringIntern(const StringIntern&) = delete;
    StringIntern& operator=(const StringIntern&) = delete;
    StringIntern(StringIntern&&) = delete;
    StringIntern& operator=(StringIntern&&) = delete;

    std::uint32_t get_or_insert(std::string_view sv) {
        const auto h = hash(sv);
        if (auto id = lookup(h, sv); id != NO_ID) return id;

        std::lock_guard lock(insert_mutex_);
        if (auto id = lookup(h, sv); id != NO_ID) return id;

        std::uint32_t id;
        if (deterministic_ids_.load(std::memory_order_acquire)) {
            id = static_cast<std::uint32_t>(h & (FAST_CAPACITY - 1));
        } else {
            id = static_cast<std::uint32_t>(
                num_strings_.load(std::memory_order_relaxed));
        }

        // Two strings on one deterministic id would merge unrelated keys.
        if (const auto* existing = slot_value(id)) {
            if (*existing == sv) return id;
            throw DFTUtilsException(
                ErrorCode::INTERNAL,
                "string intern: deterministic id collision between '" +
                    *existing + "' and '" + std::string(sv) +
                    "'; the dictionary is too large for this id space");
        }

        auto* str = new std::string(sv);
        bind_slot(id, str);
        index_insert(h, id);
        advance_num_strings(id);
        return id;
    }

    /// Insert at a specific id, for loading a persisted dictionary. Ids are
    /// index-local, so a conflicting id throws rather than rebind to another
    /// index's string. Must precede any `resolve(id)` at that id.
    void insert_at_id(std::uint32_t id, std::string_view sv) {
        const auto h = hash(sv);

        std::lock_guard lock(insert_mutex_);

        if (const auto* existing = slot_value(id)) {
            if (*existing == sv) return;
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "string intern: id " + std::to_string(id) + " is already '" +
                    *existing + "', cannot rebind to '" + std::string(sv) +
                    "' (dictionaries from two indexes in one table?)");
        }

        if (auto existing_id = lookup(h, sv); existing_id != NO_ID) {
            bind_slot(id, slot_value(existing_id));
        } else {
            bind_slot(id, new std::string(sv));
            index_insert(h, id);
        }
        advance_num_strings(id);
    }

    /// Empty means "no string at this id"; an id past the table's reach is a
    /// corrupt or foreign key, not an absent string.
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

    /// Number of strings interned, and the id of the n-th in insertion order.
    /// Content-derived ids are sparse, so enumerating the dictionary walks
    /// these rather than the id range.
    std::size_t entry_count() const {
        return entry_count_.load(std::memory_order_acquire);
    }

    std::uint32_t entry_id(std::size_t n) const {
        auto* block = log_[n >> BLOCK_BITS].load(std::memory_order_acquire);
        return block
                   ? block[n & (BLOCK_SIZE - 1)].load(std::memory_order_relaxed)
                   : 0;
    }

    /// The string bound to `id`, or null when the slot is free.
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

    /// Shift the next-to-assign id counter to `base`. Must precede any
    /// `get_or_insert`; the caller ensures no concurrent inserts.
    void reserve_id_base(std::uint32_t base) noexcept {
        num_strings_.store(base, std::memory_order_release);
    }

    /// Derive ids from string content instead of a counter, so keys embedding
    /// them match across MPI ranks. The id space is finite, so this only holds
    /// for dictionaries small enough to avoid a birthday collision; a collision
    /// throws. Must precede any `get_or_insert`.
    void enable_deterministic_ids() noexcept {
        deterministic_ids_.store(true, std::memory_order_release);
    }

   private:
    /// Open-addressed string -> id lookup; slots hold `id + 1`, so 0 is free.
    /// Growth publishes a replacement and never writes the old one again, so a
    /// reader on it stays correct and at worst misses a newer string, which
    /// sends it down the locked path.
    struct Index {
        std::size_t mask;
        IndexSlot* slots;
    };

    static constexpr std::size_t INITIAL_INDEX_SLOTS = 1u << 10;

    static Index* make_index(std::size_t slots) {
        auto* idx = new Index{slots - 1, new IndexSlot[slots]};
        for (std::size_t i = 0; i < slots; ++i)
            idx->slots[i].store(0, std::memory_order_relaxed);
        return idx;
    }

    static void delete_index(Index* idx) {
        delete[] idx->slots;
        delete idx;
    }

    std::uint32_t lookup(std::size_t h, std::string_view sv) const {
        const auto* idx = index_.load(std::memory_order_acquire);
        for (std::size_t i = h & idx->mask;; i = (i + 1) & idx->mask) {
            auto v = idx->slots[i].load(std::memory_order_acquire);
            if (v == 0) return NO_ID;
            const auto* s = slot_value(v - 1);
            if (s && *s == sv) return v - 1;
        }
    }

    /// Caller holds `insert_mutex_`.
    void index_insert(std::size_t h, std::uint32_t id) {
        auto* idx = index_.load(std::memory_order_relaxed);
        if ((index_size_ + 1) * 4 >= (idx->mask + 1) * 3) {
            idx = grow_index();
        }
        for (std::size_t i = h & idx->mask;; i = (i + 1) & idx->mask) {
            if (idx->slots[i].load(std::memory_order_relaxed) == 0) {
                idx->slots[i].store(id + 1, std::memory_order_release);
                break;
            }
        }
        ++index_size_;
    }

    Index* grow_index() {
        auto* old = index_.load(std::memory_order_relaxed);
        auto* fresh = make_index((old->mask + 1) * 2);
        for (std::size_t i = 0; i <= old->mask; ++i) {
            auto v = old->slots[i].load(std::memory_order_relaxed);
            if (v == 0) continue;
            const auto h = hash(*slot_value(v - 1));
            for (std::size_t j = h & fresh->mask;; j = (j + 1) & fresh->mask) {
                if (fresh->slots[j].load(std::memory_order_relaxed) == 0) {
                    fresh->slots[j].store(v, std::memory_order_relaxed);
                    break;
                }
            }
        }
        retired_.push_back(old);
        index_.store(fresh, std::memory_order_release);
        return fresh;
    }

    /// Caller holds `insert_mutex_`.
    void advance_num_strings(std::uint32_t id) {
        const std::size_t need = static_cast<std::size_t>(id) + 1;
        if (need > num_strings_.load(std::memory_order_relaxed)) {
            num_strings_.store(need, std::memory_order_release);
        }
    }

    /// Caller holds `insert_mutex_`.
    void log_entry(std::uint32_t id) {
        const auto n = entry_count_.load(std::memory_order_relaxed);
        auto& dir = log_[n >> BLOCK_BITS];
        auto* block = dir.load(std::memory_order_acquire);
        if (!block) {
            block = new LogSlot[BLOCK_SIZE];
            dir.store(block, std::memory_order_release);
        }
        block[n & (BLOCK_SIZE - 1)].store(id, std::memory_order_relaxed);
        entry_count_.store(n + 1, std::memory_order_release);
    }

    /// Caller holds `insert_mutex_`.
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
        log_entry(id);
    }

    // Fixed, unlike std::hash, whose result varies by standard library:
    // deterministic ids must agree across independently built processes.
    static std::size_t hash(std::string_view sv) {
        return hash::fnv1a_mix(hash::fnv1a_hash(sv));
    }

    std::unique_ptr<std::atomic<Slot*>[]> directory_;
    std::unique_ptr<std::atomic<LogSlot*>[]> log_;
    std::atomic<Index*> index_{nullptr};
    std::vector<Index*> retired_;
    std::size_t index_size_ = 0;
    std::atomic<std::size_t> entry_count_{0};
    std::atomic<std::size_t> num_strings_{0};
    std::atomic<bool> deterministic_ids_{false};
    std::mutex insert_mutex_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
