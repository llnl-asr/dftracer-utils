#ifndef DFTRACER_UTILS_TESTS_MMIX_LCG_H
#define DFTRACER_UTILS_TESTS_MMIX_LCG_H

#include <cstdint>

// One step of the 64-bit LCG with Knuth's MMIX constants (TAOCP vol. 2, 3.3.4).
inline std::uint64_t mmix_lcg_step(std::uint64_t state) {
    return state * 6364136223846793005ULL + 1442695040888963407ULL;
}

#endif  // DFTRACER_UTILS_TESTS_MMIX_LCG_H
