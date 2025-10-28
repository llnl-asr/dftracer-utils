#ifndef DFTRACER_UTILS_CORE_COMMON_TYPEDEFS_H
#define DFTRACER_UTILS_CORE_COMMON_TYPEDEFS_H

#include <cstdint>

namespace dftracer::utils {

// TaskIndex must be able to hold pointer values (for pointer-based IDs)
// Use intptr_t to ensure it's the same size as a pointer
typedef std::intptr_t TaskIndex;

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_TYPEDEFS_H
