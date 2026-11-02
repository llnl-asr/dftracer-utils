// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_INSTRUMENTATION_H
#define DFTRACER_UTILS_TESTS_INSTRUMENTATION_H

#include <cstdlib>
#include <cstring>

namespace instrumentation {

/// Peak RSS under a sanitizer or Valgrind is the tool's shadow memory, not
/// the engine's, so a memory bound means nothing there.
inline bool active() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
    return true;
#endif
#endif
    const char* preload = std::getenv("LD_PRELOAD");
    return preload != nullptr && std::strstr(preload, "vgpreload") != nullptr;
}

}  // namespace instrumentation

#endif  // DFTRACER_UTILS_TESTS_INSTRUMENTATION_H
