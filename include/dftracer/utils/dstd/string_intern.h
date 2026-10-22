#ifndef DFTRACER_UTILS_DSTD_STRING_INTERN_H
#define DFTRACER_UTILS_DSTD_STRING_INTERN_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace dftracer::utils::memory {
class Pool;
}

namespace dftracer::utils::dstd {

class StringIntern {
    using Slot = std::atomic<const char*>;
    using LogSlot = std::atomic<std::uint32_t>;
    using IndexSlot = std::atomic<std::uint64_t>;

   public:
    /// id -> string lives in lazily allocated blocks behind a two-level
    /// directory, so a small table costs a few KB rather than the id space.
    static constexpr std::size_t BLOCK_BITS = 10;
    static constexpr std::size_t BLOCK_SIZE = 1u << BLOCK_BITS;
    static constexpr std::size_t DIRECTORY_SIZE = 1u << 18;
    static constexpr std::size_t FAST_CAPACITY = BLOCK_SIZE * DIRECTORY_SIZE;

    static constexpr std::uint32_t NO_ID = UINT32_MAX;

    /// Chunks start at FIRST_CHUNK_BYTES and double up to CHUNK_BYTES, so a
    /// small table stays small; a record larger than CHUNK_BYTES gets a chunk
    /// of its own size. Records are `{u32 len, u32 chunk, bytes}` padded to 4.
    static constexpr std::size_t FIRST_CHUNK_BYTES = std::size_t{1} << 7;
    static constexpr std::size_t CHUNK_BYTES = std::size_t{1} << 20;

    /// Owned bytes that a column can hold by `shared_ptr`. `size()` is the
    /// capacity; take views only inside records that `locate` returned.
    class Chunk {
       public:
        Chunk(std::size_t size, memory::Pool& pool);
        ~Chunk();
        Chunk(const Chunk&) = delete;
        Chunk& operator=(const Chunk&) = delete;

        const char* data() const noexcept { return data_; }
        std::size_t size() const noexcept { return size_; }

       private:
        char* data_;
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

    enum class InsertStatus {
        Ok,
        /// The id holds another string.
        Conflict,
        /// The id is past FAST_CAPACITY.
        OutOfRange,
    };

    /// Every directory, block, index and chunk is charged to @p pool, or to the
    /// general pool when it is null, whatever thread allocates. The pool must
    /// stay open until the table and every chunk it handed out are gone.
    explicit StringIntern(memory::Pool* pool = nullptr);
    ~StringIntern();

    StringIntern(const StringIntern&) = delete;
    StringIntern& operator=(const StringIntern&) = delete;
    StringIntern(StringIntern&&) = delete;
    StringIntern& operator=(StringIntern&&) = delete;

    /// Throws std::logic_error on a deterministic id collision and
    /// std::length_error when the chunk table or the id space is exhausted.
    std::uint32_t get_or_insert(std::string_view sv);

    /// Insert at a specific id, for loading a persisted dictionary. Ids are
    /// index-local, so a conflicting id is reported rather than rebound to
    /// another index's string; `resolve(id)` then names the string it holds.
    /// Must precede any `resolve(id)` at that id.
    [[nodiscard]] InsertStatus insert_at_id(std::uint32_t id,
                                            std::string_view sv);

    /// Empty means "no string at this id"; an id past the table's reach is a
    /// corrupt or foreign key, not an absent string, and throws
    /// std::out_of_range.
    std::string_view resolve(std::uint32_t id) const;

    Location locate(std::uint32_t id) const;

    /// Valid for any chunk index that `locate` returned; the chunk outlives
    /// the intern while the returned pointer is held.
    std::shared_ptr<Chunk> chunk(std::uint32_t i) const;

    /// Number of strings interned, and the id of the n-th in insertion order.
    /// Content-derived ids are sparse, so enumerating the dictionary walks
    /// these rather than the id range.
    std::size_t entry_count() const;

    std::uint32_t entry_id(std::size_t n) const;

    std::string_view intern(std::string_view sv);

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

    static constexpr std::size_t SHARDS = 64;
    static_assert((SHARDS & (SHARDS - 1)) == 0);
    static constexpr std::size_t INITIAL_INDEX_SLOTS = 1u << 4;

    /// Open-addressed string -> id lookup; slots hold `(low 32 hash bits <<
    /// 32) | (id + 1)`, so 0 is free and growth never rehashes a string.
    /// Growth publishes a replacement and never writes the old one again.
    struct Index {
        std::size_t mask;
        IndexSlot* slots;
        Index* retired_next;
    };

    struct alignas(128) Shard {
        std::mutex mu;
        std::atomic<Index*> index{nullptr};
        Index* retired = nullptr;
        std::size_t index_size = 0;
        std::size_t cur_used = 0;
        std::size_t cur_size = 0;
        std::uint32_t cur = 0;
        bool have_cur = false;
    };

    static void release_bytes(void* p, std::size_t bytes) noexcept;

    /// N entries in two levels: a small top array and leaves of LEAF entries
    /// made on first use, so a table that holds few strings costs a few KB
    /// rather than the whole directory. Entries never move once made.
    template <class Block, std::size_t N>
    struct Directory {
        static constexpr std::size_t LEAF_BITS = 10;
        static constexpr std::size_t LEAF = std::size_t{1} << LEAF_BITS;
        static constexpr std::size_t TOP = (N + LEAF - 1) / LEAF;
        using Entry = std::atomic<Block*>;

        Block* load(std::size_t i) const noexcept {
            const Entry* leaf =
                top[i >> LEAF_BITS].load(std::memory_order_acquire);
            return leaf ? leaf[i & (LEAF - 1)].load(std::memory_order_acquire)
                        : nullptr;
        }

        std::array<std::atomic<Entry*>, TOP> top{};
    };

    template <class T>
    class Array {
       public:
        explicit Array(T* p, std::size_t n) noexcept : p_(p), n_(n) {}
        Array(const Array&) = delete;
        Array& operator=(const Array&) = delete;
        ~Array() {
            if (!p_) return;
            std::destroy_n(p_, n_);
            release_bytes(p_, n_ * sizeof(T));
        }
        T& operator[](std::size_t i) const noexcept { return p_[i]; }

       private:
        T* p_;
        std::size_t n_;
    };

    const char* slot_value(std::uint32_t id) const;
    static std::string_view record_view(const char* rec);
    static std::uint32_t record_chunk(const char* rec);
    const ChunkRef* chunk_ref(std::uint32_t i) const;
    static std::size_t shard_of(std::size_t h);
    [[noreturn]] static void throw_collision(const char* existing,
                                             std::string_view sv);
    std::uint32_t add_chunk(std::size_t bytes);
    const char* write_record(Shard& sh, std::string_view sv);
    Index* make_index(std::size_t slots);
    static void delete_index(Index* idx);
    std::uint32_t lookup(std::size_t h, std::string_view sv) const;
    void index_insert(Shard& sh, std::size_t h, std::uint32_t id);
    Index* grow_index(Shard& sh);
    void advance_num_strings(std::uint32_t id);
    void log_entry(std::uint32_t id);
    bool bind_slot(std::uint32_t id, const char* str, bool cas);
    std::size_t hash(std::string_view sv) const;
    template <class T>
    T* make_array(std::size_t n) const;
    template <class T>
    static void free_array(T* a, std::size_t n) noexcept;
    template <class T>
    Array<T> make_owned(std::size_t n) const;
    template <class Block, std::size_t N>
    typename Directory<Block, N>::Entry& entry(Directory<Block, N>& dir,
                                               std::size_t i);
    template <class Block, std::size_t N>
    void free_directory(Directory<Block, N>& dir,
                        std::size_t block_size) noexcept;
    std::shared_ptr<Chunk> make_chunk(std::size_t bytes);

    memory::Pool* pool_;
    Directory<Slot, DIRECTORY_SIZE> directory_;
    Directory<LogSlot, DIRECTORY_SIZE> log_;
    Directory<ChunkRef, CHUNK_DIRECTORY_SIZE> chunk_dir_;
    Array<Shard> shards_;
    std::atomic<std::size_t> num_chunks_{0};
    std::atomic<std::size_t> log_next_{0};
    mutable std::atomic<std::size_t> entry_count_{0};
    std::atomic<std::size_t> num_strings_{0};
    std::atomic<bool> deterministic_ids_{false};
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_STRING_INTERN_H
