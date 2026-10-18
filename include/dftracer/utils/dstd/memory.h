#ifndef DFTRACER_UTILS_DSTD_MEMORY_H
#define DFTRACER_UTILS_DSTD_MEMORY_H

#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/heap.h>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/// Destroys and frees with the size of T, so a UniquePtr<Derived> does not
/// convert to a UniquePtr<Base>: the sized free would get the wrong size.
template <class T>
struct Delete {
    static_assert(!std::is_array_v<T>, "UniquePtr has no array form");

    void operator()(T* p) const noexcept {
        p->~T();
        memory::deallocate(const_cast<std::remove_cv_t<T>*>(p), sizeof(T));
    }
};

template <class T>
using UniquePtr = std::unique_ptr<T, Delete<T>>;

template <class T, class... Args>
UniquePtr<T> make_unique(Args&&... args) {
    static_assert(!std::is_array_v<T>, "UniquePtr has no array form");
    void* raw = memory::allocate(sizeof(T), alignof(T));
    try {
        return UniquePtr<T>(::new (raw) T(std::forward<Args>(args)...));
    } catch (...) {
        memory::deallocate(raw, sizeof(T));
        throw;
    }
}

template <class T>
using SharedPtr = std::shared_ptr<T>;

/// One block for the object and its control block, from memory::allocate.
template <class T, class... Args>
SharedPtr<T> make_shared(Args&&... args) {
    return std::allocate_shared<T>(memory::Allocator<T>{},
                                   std::forward<Args>(args)...);
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_MEMORY_H
