#ifndef DFTRACER_UTILS_CORE_COMMON_NO_DESTRUCTOR_H
#define DFTRACER_UTILS_CORE_COMMON_NO_DESTRUCTOR_H

#include <new>
#include <utility>

namespace dftracer::utils {

/**
 * @brief Holds a T in aligned in-object storage, constructed on first use and
 * never destructed.
 *
 * For a function-local static owning process-lifetime state whose destructor at
 * exit would be unsafe (e.g. it races another library's static teardown). The T
 * lives in the static's own storage, so there is no heap allocation to leak;
 * Valgrind sees it as still-reachable static memory, not a definite/indirect
 * leak. Access with `*m` / `m->`.
 */
template <class T>
class NoDestructor {
   public:
    template <class... Args>
    explicit NoDestructor(Args&&... args) {
        ::new (storage_) T(std::forward<Args>(args)...);
    }
    NoDestructor(const NoDestructor&) = delete;
    NoDestructor& operator=(const NoDestructor&) = delete;

    T& operator*() { return *get(); }
    T* operator->() { return get(); }
    T* get() { return std::launder(reinterpret_cast<T*>(storage_)); }

   private:
    alignas(T) unsigned char storage_[sizeof(T)];
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_NO_DESTRUCTOR_H
