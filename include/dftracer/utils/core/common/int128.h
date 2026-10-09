#ifndef DFTRACER_UTILS_CORE_COMMON_INT128_H
#define DFTRACER_UTILS_CORE_COMMON_INT128_H

namespace dftracer::utils {

// The 128-bit integers of GCC and Clang, the one place they are named: wide
// sums and differences that must not wrap before a range check, and decimal
// arithmetic. `__extension__` keeps -Wpedantic quiet. A compiler without them
// needs a portable pair of 64-bit words here, and nowhere else.
__extension__ typedef __int128 int128_t;
__extension__ typedef unsigned __int128 uint128_t;

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_INT128_H
