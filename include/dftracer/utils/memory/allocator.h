#ifndef DFTRACER_UTILS_MEMORY_ALLOCATOR_H
#define DFTRACER_UTILS_MEMORY_ALLOCATOR_H

#include <dftracer/utils/memory/heap.h>

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace dftracer::utils::memory {

/**
 * @brief A standard allocator over allocate() and the sized deallocate().
 *
 * Blocks are charged to the scope pool of the thread that allocates them.
 * Empty, and every instance compares equal.
 */
template <class T>
class Allocator {
   public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    Allocator() noexcept = default;
    template <class U>
    constexpr Allocator(const Allocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return static_cast<T*>(memory::allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        memory::deallocate(p, n * sizeof(T));
    }
};

template <class T, class U>
constexpr bool operator==(const Allocator<T>&, const Allocator<U>&) noexcept {
    return true;
}

/**
 * @brief A standard allocator that charges every block to one pool, whatever
 * the scope.
 *
 * Two compare equal when they name the same pool, and the allocator follows
 * the container on copy, move and swap. The pool must be open while the
 * allocator allocates; a block that outlives the pool's handle is reported as
 * a leak when the pool closes.
 */
template <class T>
class PoolAllocator {
   public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    explicit PoolAllocator(Pool& pool) noexcept : pool_(&pool) {}
    template <class U>
    constexpr PoolAllocator(const PoolAllocator<U>& other) noexcept
        : pool_(&other.pool()) {}

    Pool& pool() const noexcept { return *pool_; }

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return static_cast<T*>(
            memory::allocate(n * sizeof(T), alignof(T), *pool_));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        memory::deallocate(p, n * sizeof(T));
    }

   private:
    Pool* pool_;
};

template <class T, class U>
constexpr bool operator==(const PoolAllocator<T>& a,
                          const PoolAllocator<U>& b) noexcept {
    return &a.pool() == &b.pool();
}

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_ALLOCATOR_H
