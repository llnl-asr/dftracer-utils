#ifndef DFTRACER_UTILS_DSTD_SINGLETON_H
#define DFTRACER_UTILS_DSTD_SINGLETON_H

#include <dftracer/utils/dstd/no_destructor.h>

namespace dftracer::utils::dstd {

/// The one T of a process, made on first use by the first caller.
///
/// No static destruction runs it, so a holder of process state can rely on it
/// being there at exit, and callers give it a shutdown hook for its teardown.
template <class T>
class Singleton {
   public:
    static T& get() {
        static NoDestructor<T> value;
        return *value;
    }
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_SINGLETON_H
