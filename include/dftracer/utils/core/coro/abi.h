#ifndef DFTRACER_UTILS_CORE_CORO_ABI_H
#define DFTRACER_UTILS_CORE_CORO_ABI_H

#include <dftracer/utils/core/common/export.h>
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

/* Create a runtime with `threads` compute workers and `io_threads` I/O
 * workers; 0 (or negative) for either means hardware_concurrency, matching
 * Runtime(0). Freed with dftu_runtime_free.
 *
 * If no default runtime is installed yet, or the installed default is the
 * lazy fallback dftu_default_runtime creates on first use, this instance
 * becomes the new default (first-user-wins). A default already installed by
 * an earlier dftu_runtime_new or dftu_set_default_runtime call is left in
 * place; use dftu_set_default_runtime to override it explicitly. */
DFTU_EXPORT dftu_runtime* dftu_runtime_new(int32_t threads, int32_t io_threads);

/* Destroy a runtime created by dftu_runtime_new. If rt is the current
 * default, the default is cleared first so a later NULL-means-default call
 * falls back to the lazy default instead of dereferencing freed memory. */
DFTU_EXPORT void dftu_runtime_free(dftu_runtime* rt);

/* Install rt as the default runtime unconditionally, replacing any current
 * default (user-installed or lazy); NULL reverts to the lazy fallback. Does
 * not take ownership of rt. */
DFTU_EXPORT void dftu_set_default_runtime(dftu_runtime* rt);

/* Drive t to completion on rt, blocking the caller (pool-safe when reentrant).
   A NULL rt runs on the default runtime, as elsewhere in this ABI. Consumes t.
   0 on success, -1 if it threw or t is NULL. */
int dftu_task_run(dftu_runtime* rt, dftu_task* t);

/* Combine n tasks into one; consumes the inputs. NULL if n == 0. */
dftu_task* dftu_task_when_all(dftu_task* const* ts, uint32_t n);
dftu_task* dftu_task_when_any(dftu_task* const* ts, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_CORE_CORO_ABI_H */
