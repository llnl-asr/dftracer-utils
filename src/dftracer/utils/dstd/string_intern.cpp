#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/manager.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace dftracer::utils::dstd {

namespace {
constexpr std::size_t CHUNK_ALIGN = 8;
}  // namespace

StringIntern::Chunk::Chunk(std::size_t size, memory::Pool& pool)
    : data_(static_cast<char*>(memory::allocate(size, CHUNK_ALIGN, pool))),
      size_(size) {}

StringIntern::Chunk::~Chunk() { memory::deallocate(data_, size_); }

void StringIntern::release_bytes(void* p, std::size_t bytes) noexcept {
    memory::deallocate(p, bytes);
}

StringIntern::StringIntern(memory::Pool* pool)
    : pool_(pool ? pool : &memory::Manager::get().general()),
      shards_(make_owned<Shard>(SHARDS)) {}

StringIntern::~StringIntern() {
    for (std::size_t s = 0; s < SHARDS; ++s) {
        if (auto* i = shards_[s].index.load(std::memory_order_relaxed))
            delete_index(i);
        for (auto* old = shards_[s].retired; old;) {
            auto* next = old->retired_next;
            delete_index(old);
            old = next;
        }
    }
    free_directory(chunk_dir_, CHUNK_BLOCK_SIZE);
    free_directory(directory_, BLOCK_SIZE);
    free_directory(log_, BLOCK_SIZE);
}

std::uint32_t StringIntern::get_or_insert(std::string_view sv) {
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

StringIntern::InsertStatus StringIntern::insert_at_id(std::uint32_t id,
                                                      std::string_view sv) {
    if (id >= FAST_CAPACITY) return InsertStatus::OutOfRange;

    const auto h = hash(sv);
    Shard& sh = shards_[shard_of(h)];
    std::lock_guard lock(sh.mu);

    if (const auto* existing = slot_value(id)) {
        return record_view(existing) == sv ? InsertStatus::Ok
                                           : InsertStatus::Conflict;
    }

    if (auto existing_id = lookup(h, sv); existing_id != NO_ID) {
        bind_slot(id, slot_value(existing_id), true);
    } else {
        bind_slot(id, write_record(sh, sv), true);
        index_insert(sh, h, id);
    }
    advance_num_strings(id);
    return InsertStatus::Ok;
}

std::string_view StringIntern::resolve(std::uint32_t id) const {
    if (id >= FAST_CAPACITY) {
        throw std::out_of_range("string intern: id " + std::to_string(id) +
                                " is out of range");
    }
    const auto* p = slot_value(id);
    return p ? record_view(p) : std::string_view{};
}

StringIntern::Location StringIntern::locate(std::uint32_t id) const {
    const auto* p = id < FAST_CAPACITY ? slot_value(id) : nullptr;
    if (!p) return {nullptr, 0, 0, 0};
    const auto v = record_view(p);
    const auto c = record_chunk(p);
    return {v.data(), static_cast<std::uint32_t>(v.size()), c,
            static_cast<std::uint32_t>(v.data() - chunk_ref(c)->get()->data())};
}

std::shared_ptr<StringIntern::Chunk> StringIntern::chunk(
    std::uint32_t i) const {
    return *chunk_ref(i);
}

std::size_t StringIntern::entry_count() const {
    auto n = entry_count_.load(std::memory_order_acquire);
    const auto start = n;
    for (;; ++n) {
        auto* block = log_.load(n >> BLOCK_BITS);
        if (!block ||
            block[n & (BLOCK_SIZE - 1)].load(std::memory_order_acquire) == 0)
            break;
    }
    auto seen = start;
    while (seen < n &&
           !entry_count_.compare_exchange_weak(
               seen, n, std::memory_order_release, std::memory_order_relaxed)) {
    }
    return n;
}

std::uint32_t StringIntern::entry_id(std::size_t n) const {
    auto* block = log_.load(n >> BLOCK_BITS);
    if (!block) return 0;
    const auto v = block[n & (BLOCK_SIZE - 1)].load(std::memory_order_acquire);
    return v == 0 ? 0 : v - 1;
}

std::string_view StringIntern::intern(std::string_view sv) {
    return resolve(get_or_insert(sv));
}

/// The record bound to `id`, or null when the slot is free.
const char* StringIntern::slot_value(std::uint32_t id) const {
    if (id >= FAST_CAPACITY) return nullptr;
    auto* block = directory_.load(id >> BLOCK_BITS);
    if (!block) return nullptr;
    return block[id & (BLOCK_SIZE - 1)].load(std::memory_order_acquire);
}

std::string_view StringIntern::record_view(const char* rec) {
    std::uint32_t len;
    std::memcpy(&len, rec, sizeof len);
    return {rec + RECORD_HEADER, len};
}

std::uint32_t StringIntern::record_chunk(const char* rec) {
    std::uint32_t c;
    std::memcpy(&c, rec + 4, sizeof c);
    return c;
}

/// An entry is written once before any record in its chunk is published, so
/// a reader that saw such a record may read it.
const StringIntern::ChunkRef* StringIntern::chunk_ref(std::uint32_t i) const {
    auto* block = chunk_dir_.load(i >> CHUNK_BLOCK_BITS);
    return &block[i & (CHUNK_BLOCK_SIZE - 1)];
}

std::size_t StringIntern::shard_of(std::size_t h) {
    return (h >> 40) & (SHARDS - 1);
}

void StringIntern::throw_collision(const char* existing, std::string_view sv) {
    throw std::logic_error(
        "string intern: deterministic id collision between '" +
        std::string(record_view(existing)) + "' and '" + std::string(sv) +
        "'; the dictionary is too large for this id space");
}

std::uint32_t StringIntern::add_chunk(std::size_t bytes) {
    const auto n = num_chunks_.fetch_add(1, std::memory_order_relaxed);
    if (n >= CHUNK_DIRECTORY_SIZE * CHUNK_BLOCK_SIZE) {
        throw std::length_error("string intern: exhausted the chunk table");
    }
    auto& dir = entry(chunk_dir_, n >> CHUNK_BLOCK_BITS);
    auto* block = dir.load(std::memory_order_acquire);
    if (!block) {
        auto* fresh = make_array<ChunkRef>(CHUNK_BLOCK_SIZE);
        if (dir.compare_exchange_strong(block, fresh,
                                        std::memory_order_acq_rel)) {
            block = fresh;
        } else {
            free_array(fresh, CHUNK_BLOCK_SIZE);
        }
    }
    block[n & (CHUNK_BLOCK_SIZE - 1)] = make_chunk(bytes);
    return static_cast<std::uint32_t>(n);
}

/// Caller holds the shard's mutex. Publish the result through a slot.
const char* StringIntern::write_record(Shard& sh, std::string_view sv) {
    const std::size_t need = (RECORD_HEADER + sv.size() + 3) & ~std::size_t{3};
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

StringIntern::Index* StringIntern::make_index(std::size_t slots) {
    auto* array = make_array<IndexSlot>(slots);
    try {
        auto* idx = make_array<Index>(1);
        *idx = Index{slots - 1, array, nullptr};
        return idx;
    } catch (...) {
        free_array(array, slots);
        throw;
    }
}

void StringIntern::delete_index(Index* idx) {
    free_array(idx->slots, idx->mask + 1);
    free_array(idx, 1);
}

std::uint32_t StringIntern::lookup(std::size_t h, std::string_view sv) const {
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
void StringIntern::index_insert(Shard& sh, std::size_t h, std::uint32_t id) {
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

StringIntern::Index* StringIntern::grow_index(Shard& sh) {
    auto* old = sh.index.load(std::memory_order_relaxed);
    auto* fresh = make_index((old->mask + 1) * 2);
    for (std::size_t i = 0; i <= old->mask; ++i) {
        auto v = old->slots[i].load(std::memory_order_relaxed);
        if (v == 0) continue;
        const auto h32 = static_cast<std::size_t>(v >> 32);
        for (std::size_t j = h32 & fresh->mask;; j = (j + 1) & fresh->mask) {
            if (fresh->slots[j].load(std::memory_order_relaxed) == 0) {
                fresh->slots[j].store(v, std::memory_order_relaxed);
                break;
            }
        }
    }
    old->retired_next = sh.retired;
    sh.retired = old;
    sh.index.store(fresh, std::memory_order_release);
    return fresh;
}

void StringIntern::advance_num_strings(std::uint32_t id) {
    const std::size_t need = static_cast<std::size_t>(id) + 1;
    auto cur = num_strings_.load(std::memory_order_relaxed);
    while (need > cur && !num_strings_.compare_exchange_weak(
                             cur, need, std::memory_order_release,
                             std::memory_order_relaxed)) {
    }
}

/// A slot holds `id + 1`, so a reader stops at the first empty slot and
/// never sees a hole below the count it returns.
void StringIntern::log_entry(std::uint32_t id) {
    const auto n = log_next_.fetch_add(1, std::memory_order_relaxed);
    auto& dir = entry(log_, n >> BLOCK_BITS);
    auto* block = dir.load(std::memory_order_acquire);
    if (!block) {
        auto* fresh = make_array<LogSlot>(BLOCK_SIZE);
        if (dir.compare_exchange_strong(block, fresh,
                                        std::memory_order_acq_rel)) {
            block = fresh;
        } else {
            free_array(fresh, BLOCK_SIZE);
        }
    }
    block[n & (BLOCK_SIZE - 1)].store(id + 1, std::memory_order_release);
}

/// False when `id` is already bound to another record.
bool StringIntern::bind_slot(std::uint32_t id, const char* str, bool cas) {
    if (id >= FAST_CAPACITY) {
        throw std::length_error("string intern: exhausted the id space at " +
                                std::to_string(id));
    }
    auto& dir = entry(directory_, id >> BLOCK_BITS);
    auto* block = dir.load(std::memory_order_acquire);
    if (!block) {
        auto* fresh = make_array<Slot>(BLOCK_SIZE);
        if (dir.compare_exchange_strong(block, fresh,
                                        std::memory_order_acq_rel)) {
            block = fresh;
        } else {
            free_array(fresh, BLOCK_SIZE);
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
std::size_t StringIntern::hash(std::string_view sv) {
    return utils::hash::fnv1a_mix(utils::hash::fnv1a_hash(sv));
}

template <class T>
T* StringIntern::make_array(std::size_t n) const {
    void* p = memory::allocate(n * sizeof(T), alignof(T), *pool_);
    auto* a = static_cast<T*>(p);
    try {
        std::uninitialized_value_construct_n(a, n);
    } catch (...) {
        memory::deallocate(p, n * sizeof(T));
        throw;
    }
    return a;
}

template <class T>
void StringIntern::free_array(T* a, std::size_t n) noexcept {
    if (!a) return;
    std::destroy_n(a, n);
    memory::deallocate(a, n * sizeof(T));
}

template <class T>
StringIntern::Array<T> StringIntern::make_owned(std::size_t n) const {
    return Array<T>(make_array<T>(n), n);
}

template <class Block, std::size_t N>
typename StringIntern::Directory<Block, N>::Entry& StringIntern::entry(
    Directory<Block, N>& dir, std::size_t i) {
    using D = Directory<Block, N>;
    auto& top = dir.top[i >> D::LEAF_BITS];
    auto* leaf = top.load(std::memory_order_acquire);
    if (!leaf) {
        auto* fresh = make_array<typename D::Entry>(D::LEAF);
        if (top.compare_exchange_strong(leaf, fresh,
                                        std::memory_order_acq_rel)) {
            leaf = fresh;
        } else {
            free_array(fresh, D::LEAF);
        }
    }
    return leaf[i & (D::LEAF - 1)];
}

template <class Block, std::size_t N>
void StringIntern::free_directory(Directory<Block, N>& dir,
                                  std::size_t block_size) noexcept {
    using D = Directory<Block, N>;
    for (auto& top : dir.top) {
        auto* leaf = top.load(std::memory_order_relaxed);
        if (!leaf) continue;
        for (std::size_t i = 0; i < D::LEAF; ++i)
            free_array(leaf[i].load(std::memory_order_relaxed), block_size);
        free_array(leaf, D::LEAF);
    }
}

std::shared_ptr<StringIntern::Chunk> StringIntern::make_chunk(
    std::size_t bytes) {
    return std::allocate_shared<Chunk>(memory::PoolAllocator<Chunk>(*pool_),
                                       bytes, *pool_);
}

}  // namespace dftracer::utils::dstd
