// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_VALGRIND_QUIRKS_H
#define DFTRACER_UTILS_TESTS_VALGRIND_QUIRKS_H

namespace dftu_utils_test {

/// Valgrind on arm64 emulates FRINTA, the instruction std::round compiles to,
/// as round to nearest even, so round(2.5) gives 2. A check that rounds an
/// exact tie runs everywhere else.
inline constexpr bool VALGRIND_ROUNDS_TIES_TO_EVEN =
#if defined(DFTRACER_UTILS_VALGRIND_MODE) && defined(__aarch64__)
    true;
#else
    false;
#endif

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_VALGRIND_QUIRKS_H
