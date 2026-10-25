#ifndef DFTRACER_UTILS_TESTS_NO_CORE_DUMP_H
#define DFTRACER_UTILS_TESTS_NO_CORE_DUMP_H

#include <sys/resource.h>

namespace no_core_dump {

/// For a child that crashes on purpose. CI turns core dumps on, and on macOS
/// one dump of a test process takes tens of seconds and can outlive ctest's
/// timeout while it holds the output pipe.
inline void disable() {
    const struct rlimit none{0, 0};
    ::setrlimit(RLIMIT_CORE, &none);
}

}  // namespace no_core_dump

#endif  // DFTRACER_UTILS_TESTS_NO_CORE_DUMP_H
