#ifndef DFTRACER_UTILS_COMMON_TEST_PROBES_H
#define DFTRACER_UTILS_COMMON_TEST_PROBES_H

/// Counters and switches that only tests read exist when the build sets
/// DFTRACER_UTILS_TEST_PROBES=1 (CMake option of the same name). Without it
/// the globals, their updates and the branches on them are not compiled.
#ifndef DFTRACER_UTILS_TEST_PROBES
#define DFTRACER_UTILS_TEST_PROBES 0
#endif

#endif  // DFTRACER_UTILS_COMMON_TEST_PROBES_H
