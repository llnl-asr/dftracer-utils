// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_MEMORY_H
#define DFTRACER_UTILS_DSTD_MEMORY_H

#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/heap.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/// Destroys and frees with the size of T. A T with a virtual destructor is
/// freed unsized at the start of the whole object instead, so a
/// UniquePtr<Derived> converts to a UniquePtr<Base> of such a base.
template <class T>
struct Delete {
    static_assert(!std::is_array_v<T>, "UniquePtr has no array form");

    Delete() noexcept = default;
    template <class U>
        requires(std::is_convertible_v<U*, T*> &&
                 std::has_virtual_destructor_v<T>)
    Delete(const Delete<U>& /*other*/) noexcept {}

    void operator()(T* p) const noexcept {
        if constexpr (std::has_virtual_destructor_v<T>) {
            void* block =
                dynamic_cast<void*>(const_cast<std::remove_cv_t<T>*>(p));
            p->~T();
            memory::deallocate(block);
        } else {
            p->~T();
            memory::deallocate(const_cast<std::remove_cv_t<T>*>(p), sizeof(T));
        }
    }
};

/// Frees an array from @ref make_unique or @ref make_unique_for_overwrite.
/// Only an element type with no destructor work is allowed, so no count is
/// stored and the block is freed unsized.
template <class T>
struct Delete<T[]> {
    static_assert(std::is_trivially_destructible_v<T>,
                  "UniquePtr<T[]> needs a trivially destructible T");

    void operator()(T* p) const noexcept {
        memory::deallocate(const_cast<std::remove_cv_t<T>*>(p));
    }
};

template <class T>
using UniquePtr = std::unique_ptr<T, Delete<T>>;

namespace detail {

template <class T>
std::remove_extent_t<T>* allocate_array(std::size_t n) {
    using E = std::remove_extent_t<T>;
    static_assert(std::is_trivially_destructible_v<E> &&
                      std::is_nothrow_default_constructible_v<E>,
                  "UniquePtr<T[]> needs a trivially destructible T with a "
                  "default constructor that cannot throw");
    // An overflowing count asks for more than any heap has, so allocate()
    // reports it as bad_alloc.
    const std::size_t bytes =
        n > std::numeric_limits<std::size_t>::max() / sizeof(E)
            ? std::numeric_limits<std::size_t>::max()
            : n * sizeof(E);
    return static_cast<E*>(memory::allocate(bytes, alignof(E)));
}

}  // namespace detail

template <class T, class... Args>
    requires(!std::is_array_v<T>)
UniquePtr<T> make_unique(Args&&... args) {
    void* raw = memory::allocate(sizeof(T), alignof(T));
    try {
        return UniquePtr<T>(::new (raw) T(std::forward<Args>(args)...));
    } catch (...) {
        memory::deallocate(raw, sizeof(T));
        throw;
    }
}

/// An array of @p n value-initialized elements (zero for numbers).
template <class T>
    requires std::is_unbounded_array_v<T>
UniquePtr<T> make_unique(std::size_t n) {
    using E = std::remove_extent_t<T>;
    E* p = detail::allocate_array<T>(n);
    for (std::size_t i = 0; i < n; ++i) ::new (p + i) E();
    return UniquePtr<T>(p);
}

/// An array of @p n default-initialized elements. A number is left
/// unwritten, for a buffer that is filled right after.
template <class T>
    requires std::is_unbounded_array_v<T>
UniquePtr<T> make_unique_for_overwrite(std::size_t n) {
    using E = std::remove_extent_t<T>;
    E* p = detail::allocate_array<T>(n);
    for (std::size_t i = 0; i < n; ++i) ::new (p + i) E;
    return UniquePtr<T>(p);
}

template <class T>
using SharedPtr = std::shared_ptr<T>;

template <class T>
using WeakPtr = std::weak_ptr<T>;

/// One block for the object and its control block, from memory::allocate.
template <class T, class... Args>
SharedPtr<T> make_shared(Args&&... args) {
    return std::allocate_shared<T>(memory::Allocator<T>{},
                                   std::forward<Args>(args)...);
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_MEMORY_H
