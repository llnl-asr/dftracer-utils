#ifndef DFTRACER_UTILS_DSTD_NO_DESTRUCTOR_H
#define DFTRACER_UTILS_DSTD_NO_DESTRUCTOR_H

#include <new>
#include <utility>

namespace dftracer::utils {

/**
 * @brief Holds a T in aligned in-object storage, constructed on first use and
 * never destructed.
 *
 * For a function-local static whose destructor at exit would be unsafe, such as
 * one that races another library's static teardown.
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

#endif  // DFTRACER_UTILS_DSTD_NO_DESTRUCTOR_H
