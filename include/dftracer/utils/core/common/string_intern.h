#ifndef DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
#define DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils {

class StringIntern {
    using Slot = std::atomic<const char*>;
    using LogSlot = std::atomic<std::uint32_t>;
    using IndexSlot = std::atomic<std::uint64_t>;

   public:
    /// id -> string lives in lazily allocated blocks behind a fixed directory,
    /// so an idle table costs the directory rather than the whole id space.
    static constexpr std::size_t BLOCK_BITS = 12;
    static constexpr std::size_t BLOCK_SIZE = 1u << BLOCK_BITS;
    static constexpr std::size_t DIRECTORY_SIZE = 1u << 16;
    static constexpr std::size_t FAST_CAPACITY = BLOCK_SIZE * DIRECTORY_SIZE;

    static constexpr std::uint32_t NO_ID = UINT32_MAX;

    /// Chunks start at FIRST_CHUNK_BYTES and double up to CHUNK_BYTES, so a
    /// small table stays small; a record larger than CHUNK_BYTES gets a chunk
    /// of its own size. Records are `{u32 len, u32 chunk, bytes}` padded to 4.
    static constexpr std::size_t FIRST_CHUNK_BYTES = std::size_t{1} << 12;
    static constexpr std::size_t CHUNK_BYTES = std::size_t{1} << 20;

    /// Owned bytes that a column can hold by `shared_ptr`. `size()` is the
    /// capacity; take views only inside records that `locate` returned.
    class Chunk {
       public:
        explicit Chunk(std::size_t size)
            : data_(std::make_unique_for_overwrite<char[]>(size)),
              size_(size) {}
        Chunk(const Chunk&) = delete;
        Chunk& operator=(const Chunk&) = delete;

        const char* data() const noexcept { return data_.get(); }
        std::size_t size() const noexcept { return size_; }

       private:
        std::unique_ptr<char[]> data_;
        std::size_t size_;
    };

    /// `data` is null when the id has no string; `offset` is where `data`
    /// starts inside chunk `chunk`.
    struct Location {
        const char* data;
        std::uint32_t len;
        std::uint32_t chunk;
        std::uint32_t offset;
    };

    StringIntern()
        : directory_(std::make_unique<std::atomic<Slot*>[]>(DIRECTORY_SIZE)),
          log_(std::make_unique<std::atomic<LogSlot*>[]>(DIRECTORY_SIZE)),
          chunk_dir_(std::make_unique<std::atomic<ChunkRef*>[]>(
              CHUNK_DIRECTORY_SIZE)) {
        for (std::size_t i = 0; i < DIRECTORY_SIZE; ++i) {
            directory_[i].store(nullptr, std::memory_order_relaxed);
            log_[i].store(nullptr, std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < CHUNK_DIRECTORY_SIZE; ++i)
            chunk_dir_[i].store(nullptr, std::memory_order_relaxed);
        shards_ = std::make_unique<Shard[]>(SHARDS);
    }

    ~StringIntern() {
        for (std::size_t s = 0; s < SHARDS; ++s) {
            if (auto* i = shards_[s].index.load(std::memory_order_relaxed))
                delete_index(i);
            for (auto* old : shards_[s].retired) delete_index(old);
        }
        for (std::size_t i = 0; i < CHUNK_DIRECTORY_SIZE; ++i)
            delete[] chunk_dir_[i].load(std::memory_order_relaxed);
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

        Shard& sh = shards_[shard_of(h)];
        std::lock_guard lock(sh.mu);
        if (auto id = lookup(h, sv); id != NO_ID) return id;

        const bool det = deterministic_ids_.load(std::memory_order_acquire);
        std::uint32_t id;
        if (det) {
            id = static_cast<std::uint32_t>(h & (FAST_CAPACITY - 1));
            if (const auto* existing = slot_value(id)) {
                if (record_view(existing) == sv) return id;
                throw_collision(existing, sv);
            }
        } else {
            id = static_cast<std::uint32_t>(
                num_strings_.fetch_add(1, std::memory_order_relaxed));
        }

        const char* rec = write_record(sh, sv);
        if (!bind_slot(id, rec, det)) {
            throw_collision(slot_value(id), sv);
        }
        index_insert(sh, h, id);
        if (det) advance_num_strings(id);
        return id;
    }

    /// Insert at a specific id, for loading a persisted dictionary. Ids are
    /// index-local, so a conflicting id throws rather than rebind to another
    /// index's string. Must precede any `resolve(id)` at that id.
    void insert_at_id(std::uint32_t id, std::string_view sv) {
        const auto h = hash(sv);
        Shard& sh = shards_[shard_of(h)];
        std::lock_guard lock(sh.mu);

        if (const auto* existing = slot_value(id)) {
            if (record_view(existing) == sv) return;
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "string intern: id " + std::to_string(id) + " is already '" +
                    std::string(record_view(existing)) +
                    "', cannot rebind to '" + std::string(sv) +
                    "' (dictionaries from two indexes in one table?)");
        }

        if (auto existing_id = lookup(h, sv); existing_id != NO_ID) {
            bind_slot(id, slot_value(existing_id), true);
        } else {
            bind_slot(id, write_record(sh, sv), true);
            index_insert(sh, h, id);
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
        const auto* p = slot_value(id);
        return p ? record_view(p) : std::string_view{};
    }

    Location locate(std::uint32_t id) const {
        const auto* p = id < FAST_CAPACITY ? slot_value(id) : nullptr;
        if (!p) return {nullptr, 0, 0, 0};
        const auto v = record_view(p);
        const auto c = record_chunk(p);
        return {
            v.data(), static_cast<std::uint32_t>(v.size()), c,
            static_cast<std::uint32_t>(v.data() - chunk_ref(c)->get()->data())};
    }

    /// Valid for any chunk index that `locate` returned; the chunk outlives
    /// the intern while the returned pointer is held.
    std::shared_ptr<Chunk> chunk(std::uint32_t i) const {
        return *chunk_ref(i);
    }

    /// Number of strings interned, and the id of the n-th in insertion order.
    /// Content-derived ids are sparse, so enumerating the dictionary walks
    /// these rather than the id range.
    std::size_t entry_count() const {
        auto n = entry_count_.load(std::memory_order_acquire);
        const auto start = n;
        for (;; ++n) {
            auto* block = log_[n >> BLOCK_BITS].load(std::memory_order_acquire);
            if (!block || block[n & (BLOCK_SIZE - 1)].load(
                              std::memory_order_acquire) == 0)
                break;
        }
        auto seen = start;
        while (seen < n && !entry_count_.compare_exchange_weak(
                               seen, n, std::memory_order_release,
                               std::memory_order_relaxed)) {
        }
        return n;
    }

    std::uint32_t entry_id(std::size_t n) const {
        auto* block = log_[n >> BLOCK_BITS].load(std::memory_order_acquire);
        if (!block) return 0;
        const auto v =
            block[n & (BLOCK_SIZE - 1)].load(std::memory_order_acquire);
        return v == 0 ? 0 : v - 1;
    }

    std::string_view intern(std::string_view sv) {
        return resolve(get_or_insert(sv));
    }

    std::size_t size() const {
        return num_strings_.load(std::memory_order_acquire);
    }

    /// Derive ids from string content instead of a counter, so keys embedding
    /// them match across MPI ranks. The id space is finite, so this only holds
    /// for dictionaries small enough to avoid a birthday collision; a collision
    /// throws. Must precede any `get_or_insert`.
    void enable_deterministic_ids() noexcept {
        deterministic_ids_.store(true, std::memory_order_release);
    }

   private:
    using ChunkRef = std::shared_ptr<Chunk>;

    static constexpr std::size_t CHUNK_BLOCK_BITS = 8;
    static constexpr std::size_t CHUNK_BLOCK_SIZE = 1u << CHUNK_BLOCK_BITS;
    static constexpr std::size_t CHUNK_DIRECTORY_SIZE = 1u << 12;
    static constexpr std::size_t RECORD_HEADER = 8;

    /// The record bound to `id`, or null when the slot is free.
    const char* slot_value(std::uint32_t id) const {
        if (id >= FAST_CAPACITY) return nullptr;
        auto* block =
            directory_[id >> BLOCK_BITS].load(std::memory_order_acquire);
        if (!block) return nullptr;
        return block[id & (BLOCK_SIZE - 1)].load(std::memory_order_acquire);
    }

    static std::string_view record_view(const char* rec) {
        std::uint32_t len;
        std::memcpy(&len, rec, sizeof len);
        return {rec + RECORD_HEADER, len};
    }

    static std::uint32_t record_chunk(const char* rec) {
        std::uint32_t c;
        std::memcpy(&c, rec + 4, sizeof c);
        return c;
    }

    /// An entry is written once before any record in its chunk is published, so
    /// a reader that saw such a record may read it.
    const ChunkRef* chunk_ref(std::uint32_t i) const {
        auto* block =
            chunk_dir_[i >> CHUNK_BLOCK_BITS].load(std::memory_order_acquire);
        return &block[i & (CHUNK_BLOCK_SIZE - 1)];
    }

    static constexpr std::size_t SHARDS = 64;
    static_assert((SHARDS & (SHARDS - 1)) == 0);
    static constexpr std::size_t INITIAL_INDEX_SLOTS = 1u << 8;

    /// Open-addressed string -> id lookup; slots hold `(low 32 hash bits <<
    /// 32) | (id + 1)`, so 0 is free and growth never rehashes a string.
    /// Growth publishes a replacement and never writes the old one again.
    struct Index {
        std::size_t mask;
        IndexSlot* slots;
    };

    struct alignas(128) Shard {
        std::mutex mu;
        std::atomic<Index*> index{nullptr};
        std::vector<Index*> retired;
        std::size_t index_size = 0;
        std::size_t cur_used = 0;
        std::size_t cur_size = 0;
        std::uint32_t cur = 0;
        bool have_cur = false;
    };

    static std::size_t shard_of(std::size_t h) {
        return (h >> 40) & (SHARDS - 1);
    }

    [[noreturn]] static void throw_collision(const char* existing,
                                             std::string_view sv) {
        throw DFTUtilsException(
            ErrorCode::INTERNAL,
            "string intern: deterministic id collision between '" +
                std::string(record_view(existing)) + "' and '" +
                std::string(sv) +
                "'; the dictionary is too large for this id space");
    }

    std::uint32_t add_chunk(std::size_t bytes) {
        const auto n = num_chunks_.fetch_add(1, std::memory_order_relaxed);
        if (n >= CHUNK_DIRECTORY_SIZE * CHUNK_BLOCK_SIZE) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "string intern: exhausted the chunk table");
        }
        auto& dir = chunk_dir_[n >> CHUNK_BLOCK_BITS];
        auto* block = dir.load(std::memory_order_acquire);
        if (!block) {
            auto* fresh = new ChunkRef[CHUNK_BLOCK_SIZE];
            if (dir.compare_exchange_strong(block, fresh,
                                            std::memory_order_acq_rel)) {
                block = fresh;
            } else {
                delete[] fresh;
            }
        }
        block[n & (CHUNK_BLOCK_SIZE - 1)] = std::make_shared<Chunk>(bytes);
        return static_cast<std::uint32_t>(n);
    }

    /// Caller holds the shard's mutex. Publish the result through a slot.
    const char* write_record(Shard& sh, std::string_view sv) {
        const std::size_t need =
            (RECORD_HEADER + sv.size() + 3) & ~std::size_t{3};
        std::uint32_t c;
        std::size_t off;
        if (need > CHUNK_BYTES) {
            c = add_chunk(need);
            off = 0;
        } else {
            if (!sh.have_cur || sh.cur_used + need > sh.cur_size) {
                std::size_t size = sh.have_cur
                                       ? std::min(sh.cur_size * 2, CHUNK_BYTES)
                                       : FIRST_CHUNK_BYTES;
                while (size < need) size *= 2;
                sh.cur = add_chunk(size);
                sh.cur_size = size;
                sh.have_cur = true;
                sh.cur_used = 0;
            }
            c = sh.cur;
            off = sh.cur_used;
            sh.cur_used += need;
        }
        auto* rec = const_cast<char*>(chunk_ref(c)->get()->data()) + off;
        const auto len = static_cast<std::uint32_t>(sv.size());
        std::memcpy(rec, &len, sizeof len);
        std::memcpy(rec + 4, &c, sizeof c);
        if (!sv.empty()) std::memcpy(rec + RECORD_HEADER, sv.data(), sv.size());
        return rec;
    }

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
        const auto* idx =
            shards_[shard_of(h)].index.load(std::memory_order_acquire);
        if (!idx) return NO_ID;
        const std::uint64_t tag = static_cast<std::uint32_t>(h);
        for (std::size_t i = h & idx->mask;; i = (i + 1) & idx->mask) {
            auto v = idx->slots[i].load(std::memory_order_acquire);
            if (v == 0) return NO_ID;
            if ((v >> 32) != tag) continue;
            const auto id = static_cast<std::uint32_t>(v) - 1;
            const auto* s = slot_value(id);
            if (s && record_view(s) == sv) return id;
        }
    }

    /// Caller holds the shard's mutex.
    void index_insert(Shard& sh, std::size_t h, std::uint32_t id) {
        auto* idx = sh.index.load(std::memory_order_relaxed);
        if (!idx) {
            idx = make_index(INITIAL_INDEX_SLOTS);
            sh.index.store(idx, std::memory_order_release);
        } else if ((sh.index_size + 1) * 4 >= (idx->mask + 1) * 3) {
            idx = grow_index(sh);
        }
        const std::uint64_t v =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(h)) << 32) |
            (static_cast<std::uint64_t>(id) + 1);
        for (std::size_t i = h & idx->mask;; i = (i + 1) & idx->mask) {
            if (idx->slots[i].load(std::memory_order_relaxed) == 0) {
                idx->slots[i].store(v, std::memory_order_release);
                break;
            }
        }
        ++sh.index_size;
    }

    Index* grow_index(Shard& sh) {
        auto* old = sh.index.load(std::memory_order_relaxed);
        auto* fresh = make_index((old->mask + 1) * 2);
        for (std::size_t i = 0; i <= old->mask; ++i) {
            auto v = old->slots[i].load(std::memory_order_relaxed);
            if (v == 0) continue;
            const auto h32 = static_cast<std::size_t>(v >> 32);
            for (std::size_t j = h32 & fresh->mask;;
                 j = (j + 1) & fresh->mask) {
                if (fresh->slots[j].load(std::memory_order_relaxed) == 0) {
                    fresh->slots[j].store(v, std::memory_order_relaxed);
                    break;
                }
            }
        }
        sh.retired.push_back(old);
        sh.index.store(fresh, std::memory_order_release);
        return fresh;
    }

    void advance_num_strings(std::uint32_t id) {
        const std::size_t need = static_cast<std::size_t>(id) + 1;
        auto cur = num_strings_.load(std::memory_order_relaxed);
        while (need > cur && !num_strings_.compare_exchange_weak(
                                 cur, need, std::memory_order_release,
                                 std::memory_order_relaxed)) {
        }
    }

    /// A slot holds `id + 1`, so a reader stops at the first empty slot and
    /// never sees a hole below the count it returns.
    void log_entry(std::uint32_t id) {
        const auto n = log_next_.fetch_add(1, std::memory_order_relaxed);
        auto& dir = log_[n >> BLOCK_BITS];
        auto* block = dir.load(std::memory_order_acquire);
        if (!block) {
            auto* fresh = new LogSlot[BLOCK_SIZE];
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i)
                fresh[i].store(0, std::memory_order_relaxed);
            if (dir.compare_exchange_strong(block, fresh,
                                            std::memory_order_acq_rel)) {
                block = fresh;
            } else {
                delete[] fresh;
            }
        }
        block[n & (BLOCK_SIZE - 1)].store(id + 1, std::memory_order_release);
    }

    /// False when `id` is already bound to another record.
    bool bind_slot(std::uint32_t id, const char* str, bool cas) {
        if (id >= FAST_CAPACITY) {
            throw DFTUtilsException(
                ErrorCode::INTERNAL,
                "string intern: exhausted the id space at " +
                    std::to_string(id));
        }
        auto& dir = directory_[id >> BLOCK_BITS];
        auto* block = dir.load(std::memory_order_acquire);
        if (!block) {
            auto* fresh = new Slot[BLOCK_SIZE];
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i)
                fresh[i].store(nullptr, std::memory_order_relaxed);
            if (dir.compare_exchange_strong(block, fresh,
                                            std::memory_order_acq_rel)) {
                block = fresh;
            } else {
                delete[] fresh;
            }
        }
        auto& slot = block[id & (BLOCK_SIZE - 1)];
        if (cas) {
            const char* expected = nullptr;
            if (!slot.compare_exchange_strong(expected, str,
                                              std::memory_order_acq_rel))
                return false;
        } else {
            slot.store(str, std::memory_order_release);
        }
        log_entry(id);
        return true;
    }

    /// Fixed, unlike std::hash, whose result varies by standard library:
    /// deterministic ids must agree across independently built processes.
    static std::size_t hash(std::string_view sv) {
        return hash::fnv1a_mix(hash::fnv1a_hash(sv));
    }

    std::unique_ptr<std::atomic<Slot*>[]> directory_;
    std::unique_ptr<std::atomic<LogSlot*>[]> log_;
    std::unique_ptr<std::atomic<ChunkRef*>[]> chunk_dir_;
    std::unique_ptr<Shard[]> shards_;
    std::atomic<std::size_t> num_chunks_{0};
    std::atomic<std::size_t> log_next_{0};
    mutable std::atomic<std::size_t> entry_count_{0};
    std::atomic<std::size_t> num_strings_{0};
    std::atomic<bool> deterministic_ids_{false};
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STRING_INTERN_H
