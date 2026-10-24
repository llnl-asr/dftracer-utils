#ifndef DFTRACER_UTILS_MEMORY_ARENA_H
#define DFTRACER_UTILS_MEMORY_ARENA_H

#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/manager.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dftracer::utils::memory {

inline constexpr std::size_t ARENA_FIRST_CHUNK = std::size_t{64} << 10;
inline constexpr std::size_t ARENA_MAX_CHUNK = std::size_t{4} << 20;

/**
 * @brief A bump allocator over chunks charged to its own Scratch child pool.
 *
 * Chunks are 64 KiB, doubling to 4 MiB, then 4 MiB each. A request that does
 * not fit the next chunk gets a chunk of its own, which reset() frees like
 * every chunk but the first. Blocks are never freed one
 * by one: reset() and release() free them all. A new chunk is charged to the
 * pool and counts against the standing reservation that reserve() tops up; a
 * chunk past that reservation is still taken and only adds to the pressure.
 *
 * Single owner: no call is thread safe. Neither copyable nor movable, so an
 * ArenaAllocator can hold its address.
 */
class Arena {
   public:
    /// Open the child pool @p name below @p parent. Throws std::bad_alloc when
    /// the pool table is full.
    Arena(Pool& parent, std::string_view name);
    /// Open the child pool below current_pool(), read once here.
    explicit Arena(std::string_view name);
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    /// Runs the destructor list, frees every chunk and closes the pool.
    ~Arena();

    /**
     * @brief @p n bytes aligned to @p align, valid until reset() or release().
     *
     * @p align must be a power of two; any other value is undefined, and the
     * abort when a new chunk is needed is not a guarantee. A zero @p n returns
     * a pointer that must not be read. Throws std::bad_alloc when a new chunk
     * is not available.
     */
    void* allocate(std::size_t n,
                   std::size_t align = alignof(std::max_align_t)) {
        std::uintptr_t p = (cur_ + align - 1) & ~(align - 1);
        if (p <= end_ && n <= end_ - p) [[likely]] {
            cur_ = p + n;
            asan_unpoison(reinterpret_cast<void*>(p), n);
            return reinterpret_cast<void*>(p);
        }
        return allocate_chunk(n, align);
    }

    /// Like allocate() and null instead of throwing, which happens only when
    /// the hard cap or the operating system refuses a new chunk.
    void* try_allocate(std::size_t n,
                       std::size_t align = alignof(std::max_align_t)) noexcept;

    /// Construct a T that is trivially destructible, so reset() can drop it.
    template <class T, class... Args>
    T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "use make_destroyed for a T with a destructor");
        return ::new (allocate(sizeof(T), alignof(T)))
            T(std::forward<Args>(args)...);
    }

    /// Construct a T whose destructor runs at reset(), release() and in the
    /// arena's destructor, the newest object first. The destructor may call
    /// make() and make_destroyed() on this arena, but not reset() or release().
    template <class T, class... Args>
    T* make_destroyed(Args&&... args) {
        static_assert(std::is_nothrow_destructible_v<T>);
        auto* node = static_cast<Destructor*>(
            allocate(sizeof(Destructor), alignof(Destructor)));
        T* object = ::new (allocate(sizeof(T), alignof(T)))
            T(std::forward<Args>(args)...);
        node->destroy = [](void* p) noexcept { static_cast<T*>(p)->~T(); };
        node->object = object;
        node->next = destructors_;
        destructors_ = node;
        return object;
    }

    /**
     * @brief Run the destructor list and free every chunk but the first.
     *
     * Chunk growth starts again after the first chunk. Every block becomes
     * invalid; ASan builds poison the kept chunk.
     */
    void reset() noexcept;

    /// Like reset(), frees the first chunk too and drops the standing
    /// reservation.
    void release() noexcept;

    /**
     * @brief Top up the standing reservation to the bytes the chunks hold
     * plus @p bytes.
     *
     * Returns the Pool::reserve status; on failure the reservation keeps what
     * it had.
     */
    Status reserve(std::size_t bytes) noexcept;

    Pool& pool() const noexcept { return *pool_; }
    /// Bytes of every chunk the arena holds. The pool can show more: the page
    /// heap rounds a chunk of its own above 1 MiB to a power of two.
    std::size_t chunk_bytes() const noexcept { return chunk_bytes_; }
    std::size_t reserved() const noexcept { return reservation_.bytes(); }
    /// Bytes left in the current chunk before any alignment padding.
    std::size_t room() const noexcept { return cur_ <= end_ ? end_ - cur_ : 0; }
    /// The size of the next chunk the arena takes, unless a request too big
    /// for it gets a chunk of its own. The one after doubles, up to
    /// ARENA_MAX_CHUNK.
    std::size_t next_chunk() const noexcept {
        return grown_ >= CHUNK_DOUBLINGS ? ARENA_MAX_CHUNK
                                         : ARENA_FIRST_CHUNK << grown_;
    }
    /// The bytes a chunk of each size spends on its header.
    static constexpr std::size_t CHUNK_HEADER = 64;

   private:
    struct alignas(CHUNK_HEADER) Chunk {
        Chunk* next;
        std::size_t size;
    };
    static_assert(sizeof(Chunk) == CHUNK_HEADER);
    static constexpr std::size_t CHUNK_DOUBLINGS = 6;
    static_assert((ARENA_FIRST_CHUNK << CHUNK_DOUBLINGS) == ARENA_MAX_CHUNK);

    struct Destructor {
        void (*destroy)(void*) noexcept;
        void* object;
        Destructor* next;
    };

    void* allocate_chunk(std::size_t n, std::size_t align);
    Chunk* new_chunk(std::size_t size);
    void run_destructors() noexcept;

    // An empty arena has the start past the end, so the fast path always
    // misses, even for a zero size.
    std::uintptr_t cur_ = 1;
    std::uintptr_t end_ = 0;
    Chunk* chunks_ = nullptr;
    Chunk* first_ = nullptr;
    Destructor* destructors_ = nullptr;
    std::size_t chunk_bytes_ = 0;
    std::size_t grown_ = 0;
    PoolHandle pool_;
    Reservation reservation_;
};

/**
 * @brief A standard allocator over an Arena; deallocate() does nothing.
 *
 * Two compare equal when they use the same arena, and the allocator follows
 * the container on copy, move and swap. The arena must outlive every
 * container that uses it, and reset() invalidates their storage.
 */
template <class T>
class ArenaAllocator {
   public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    explicit ArenaAllocator(Arena& arena) noexcept : arena_(&arena) {}
    template <class U>
    constexpr ArenaAllocator(const ArenaAllocator<U>& other) noexcept
        : arena_(&other.arena()) {}

    Arena& arena() const noexcept { return *arena_; }

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return static_cast<T*>(arena_->allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T*, std::size_t) noexcept {}

   private:
    Arena* arena_;
};

template <class T, class U>
constexpr bool operator==(const ArenaAllocator<T>& a,
                          const ArenaAllocator<U>& b) noexcept {
    return &a.arena() == &b.arena();
}

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_ARENA_H
