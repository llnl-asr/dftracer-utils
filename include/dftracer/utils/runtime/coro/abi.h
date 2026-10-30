#ifndef DFTRACER_UTILS_RUNTIME_CORO_ABI_H
#define DFTRACER_UTILS_RUNTIME_CORO_ABI_H

#include <dftracer/utils/common/export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DFTU_TYPEDEF_DFTU_TASK
#define DFTU_TYPEDEF_DFTU_TASK
typedef struct dftu_task dftu_task;
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

/* Drive t to completion on rt, blocking the caller (pool-safe when reentrant).
   A NULL rt runs on the default runtime, as elsewhere in this ABI. Consumes t.
   0 on success, -1 if it threw or t is NULL. */
int dftu_task_run(dftu_runtime* rt, dftu_task* t);

/* Combine n tasks into one; consumes the inputs. NULL if n == 0. */
dftu_task* dftu_task_when_all(dftu_task* const* ts, uint32_t n);
/* Like when_all, but the combined task finishes with the first input to
   finish. The other inputs are cancelled and awaited before it finishes, so
   none runs after it; an input that cannot be cancelled runs to its end
   first. */
dftu_task* dftu_task_when_any(dftu_task* const* ts, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_RUNTIME_CORO_ABI_H */
