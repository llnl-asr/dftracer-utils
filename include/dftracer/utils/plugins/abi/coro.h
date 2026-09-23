#ifndef DFTRACER_UTILS_PLUGINS_ABI_CORO_H
#define DFTRACER_UTILS_PLUGINS_ABI_CORO_H

/** @file
 * dftu.svc.coro: task combinators lent to a plugin. Optional service group,
 * fetched via dftu_plugin_host::get_service(DFTU_SVC_CORO). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_CORO "dftu.svc.coro@1"

typedef struct dftu_svc_coro {
    dftu_task* (*spawn)(void* h, dftu_work_fn fn, void* arg);
    dftu_task* (*when_all)(void* h, dftu_task* const* ts, uint32_t n);
    dftu_task* (*when_any)(void* h, dftu_task* const* ts, uint32_t n);
    dftu_task* (*then)(void* h, dftu_task* t, dftu_work_fn fn, void* arg);
    /** Drive a coroutine: host awaits each dftu_task step() yields until NULL.
     */
    dftu_task* (*drive)(void* h, dftu_task* (*step)(void* coro), void* coro);
    /** Run a synchronous blocking call fn(arg) inline while releasing the
       worker's run-permit for its duration, so a raw block (a legacy blocking
       library, a call with no async form) does not starve the elastic pool.
       Prefer the async dftu_task path for anything that has an awaitable form.
     */
    void (*run_blocking)(void* h, dftu_work_fn fn, void* arg);
} dftu_svc_coro;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_CORO_H */
