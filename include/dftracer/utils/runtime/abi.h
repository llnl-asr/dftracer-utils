// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_ABI_H
#define DFTRACER_UTILS_RUNTIME_ABI_H

#include <dftracer/utils/common/export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dftu_runtime dftu_runtime;

DFTU_EXPORT dftu_runtime* dftu_default_runtime(void);

/* The process has one runtime, the one dftu_default_runtime returns. This sets
 * its compute workers to `threads` and grows its I/O workers to `io_threads`,
 * making it first when needed. 0 or negative keeps the current count
 * (hardware_concurrency for a new runtime). */
DFTU_EXPORT dftu_runtime* dftu_runtime_new(int32_t threads, int32_t io_threads);

/* Ends a caller's use of the runtime dftu_runtime_new returned. The runtime
 * runs until the process exits, so this frees nothing; NULL is allowed. */
DFTU_EXPORT void dftu_runtime_free(dftu_runtime* rt);

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_RUNTIME_ABI_H */
