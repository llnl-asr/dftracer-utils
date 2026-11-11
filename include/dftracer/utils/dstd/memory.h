// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_MEMORY_H
#define DFTRACER_UTILS_DSTD_MEMORY_H

#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/detail/heap.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/// Base of a type that is made with a plain `new` and freed with a plain
/// `delete`. Its objects come from this library's heap and are charged to the
/// pool current at the `new`, also in a shared library or a build without the
/// global operator new. An empty base adds no size. A type with a virtual
/// destructor is fine only if every `delete` goes through that destructor, so
/// the size passed to operator delete is the size of the whole object. The
/// `new (std::nothrow)` form returns null when the heap refuses.
struct HeapObject {
    static void* operator new(std::size_t size) {
        return memory::detail::allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    }
    static void operator delete(void* ptr, std::size_t size) noexcept {
        memory::detail::deallocate(ptr, size);
    }
    static void* operator new(std::size_t size,
                              const std::nothrow_t& /*tag*/) noexcept {
        return memory::detail::try_allocate(size,
                                            __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    }
    static void operator delete(void* ptr,
                                const std::nothrow_t& /*tag*/) noexcept {
        memory::detail::deallocate(ptr);
    }
};

/// Destroys and frees with the size of T. A T with a virtual destructor is
/// freed unsized at the start of the whole object instead, so a
/// UniquePtr<Derived> converts to a UniquePtr<Base> of such a base.
template <class T>
struct Delete {
    static_assert(!std::is_array_v<T>, "UniquePtr has no array form");

    Delete() noexcept = default;
    template <class U>
    // U == T comes first: T can be incomplete where a member UniquePtr<T>
    // is declared, and the trait below is an error on an incomplete type.
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*> &&
                 std::has_virtual_destructor_v<T>)
    Delete(const Delete<U>& /*other*/) noexcept {}

    void operator()(T* p) const noexcept {
        if constexpr (std::has_virtual_destructor_v<T>) {
            void* block =
                dynamic_cast<void*>(const_cast<std::remove_cv_t<T>*>(p));
            p->~T();
            memory::detail::deallocate(block);
        } else {
            p->~T();
            memory::detail::deallocate(const_cast<std::remove_cv_t<T>*>(p),
                                       sizeof(T));
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
        memory::detail::deallocate(const_cast<std::remove_cv_t<T>*>(p));
    }
};

template <class T>
using UniquePtr = std::unique_ptr<T, Delete<T>>;

namespace detail {

template <class T>
std::remove_extent_t<T>* allocate_array(std::size_t n) {
    using E = std::remove_extent_t<T>;
    static_assert(std::is_trivially_destructible_v<E>,
                  "UniquePtr<T[]> needs a trivially destructible T");
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(E)) {
        memory::detail::size_overflow();
    }
    return static_cast<E*>(memory::detail::allocate(n * sizeof(E), alignof(E)));
}

/// Builds @p n elements in @p p with @p make. If one throws, the block is
/// freed. Built elements need no destructor, as T is trivially destructible.
template <class T, class Make>
UniquePtr<T> fill_array(std::remove_extent_t<T>* p, std::size_t n, Make make) {
#if defined(__cpp_exceptions)
    try {
        for (std::size_t i = 0; i < n; ++i) make(p + i);
    } catch (...) {
        memory::detail::deallocate(p);
        throw;
    }
#else
    for (std::size_t i = 0; i < n; ++i) make(p + i);
#endif
    return UniquePtr<T>(p);
}

}  // namespace detail

template <class T, class... Args>
    requires(!std::is_array_v<T>)
UniquePtr<T> make_unique(Args&&... args) {
    void* raw = memory::detail::allocate(sizeof(T), alignof(T));
#if defined(__cpp_exceptions)
    try {
        return UniquePtr<T>(::new (raw) T(std::forward<Args>(args)...));
    } catch (...) {
        memory::detail::deallocate(raw, sizeof(T));
        throw;
    }
#else
    return UniquePtr<T>(::new (raw) T(std::forward<Args>(args)...));
#endif
}

/// An array of @p n value-initialized elements (zero for numbers).
template <class T>
    requires std::is_unbounded_array_v<T>
UniquePtr<T> make_unique(std::size_t n) {
    using E = std::remove_extent_t<T>;
    // One call per element stays in an unoptimized build, which then pays a
    // call per byte of a large buffer.
    if constexpr (std::is_arithmetic_v<E> || std::is_enum_v<E> ||
                  std::is_pointer_v<E>) {
        E* p = detail::allocate_array<T>(n);
        std::memset(p, 0, n * sizeof(E));
        return UniquePtr<T>(p);
    } else {
        return detail::fill_array<T>(detail::allocate_array<T>(n), n,
                                     [](E* at) { ::new (at) E(); });
    }
}

/// An array of @p n default-initialized elements. A number is left
/// unwritten, for a buffer that is filled right after.
template <class T>
    requires std::is_unbounded_array_v<T>
UniquePtr<T> make_unique_for_overwrite(std::size_t n) {
    using E = std::remove_extent_t<T>;
    if constexpr (std::is_trivially_default_constructible_v<E>) {
        return UniquePtr<T>(detail::allocate_array<T>(n));
    } else {
        return detail::fill_array<T>(detail::allocate_array<T>(n), n,
                                     [](E* at) { ::new (at) E; });
    }
}

/**
 * @brief A std::shared_ptr whose count block always comes from this library's
 * heap.
 *
 * Make one with @ref make_shared, with @ref to_shared_ptr from a @ref UniquePtr
 * or with @ref adopt_shared_ptr from a pointer and its deleter. A raw pointer,
 * a pointer with only a deleter and a std::unique_ptr do not compile, because
 * std::shared_ptr would take the count block from the global operator new.
 */
template <class T>
class SharedPtr : public std::shared_ptr<T> {
   public:
    using std::shared_ptr<T>::shared_ptr;
    using std::shared_ptr<T>::reset;

    SharedPtr() noexcept = default;
    SharedPtr(const std::shared_ptr<T>& other) noexcept
        : std::shared_ptr<T>(other) {}
    SharedPtr(std::shared_ptr<T>&& other) noexcept
        : std::shared_ptr<T>(std::move(other)) {}

    template <class Y>
    explicit SharedPtr(Y*) = delete;
    template <class Y, class D>
    SharedPtr(Y*, D) = delete;
    template <class Y, class D>
    SharedPtr(std::unique_ptr<Y, D>&&) = delete;
    template <class Y>
    void reset(Y*) = delete;
    template <class Y, class D>
    void reset(Y*, D) = delete;
};

template <class T>
using WeakPtr = std::weak_ptr<T>;

/// One block for the object and its control block, from
/// memory::detail::allocate.
template <class T, class... Args>
    requires(!std::is_array_v<T>)
SharedPtr<T> make_shared(Args&&... args) {
    return std::allocate_shared<T>(memory::Allocator<T>{},
                                   std::forward<Args>(args)...);
}

/// An array of @p n value-initialized elements (zero for numbers) and its
/// control block in one block from memory::detail::allocate.
template <class T>
    requires std::is_unbounded_array_v<T>
SharedPtr<T> make_shared(std::size_t n) {
    return std::allocate_shared<T>(memory::Allocator<std::byte>{}, n);
}

/// Like make_shared<T[]>(n), but a number is left unwritten, for a buffer
/// that is filled right after.
template <class T>
    requires std::is_unbounded_array_v<T>
SharedPtr<T> make_shared_for_overwrite(std::size_t n) {
    return std::allocate_shared_for_overwrite<T>(memory::Allocator<std::byte>{},
                                                 n);
}

/// Shares the object or array of @p p. The count block comes from
/// memory::detail::allocate.
template <class T>
SharedPtr<T> to_shared_ptr(UniquePtr<T>&& p) {
    return SharedPtr<T>(p.release(), Delete<T>{},
                        memory::Allocator<std::byte>{});
}

/// Shares @p p, freed by @p deleter. The count block comes from
/// memory::detail::allocate. When that allocation fails, @p deleter frees @p p.
template <class T, class D>
SharedPtr<T> adopt_shared_ptr(T* p, D deleter) {
    return SharedPtr<T>(p, std::move(deleter), memory::Allocator<std::byte>{});
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_MEMORY_H
