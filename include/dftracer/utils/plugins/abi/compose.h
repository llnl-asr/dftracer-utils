#ifndef DFTRACER_UTILS_PLUGINS_ABI_COMPOSE_H
#define DFTRACER_UTILS_PLUGINS_ABI_COMPOSE_H

/** @file
 * dftu.svc.compose: first-class async op handles lent to a plugin. Optional
 * service group, fetched via dftu_plugin_host::get_service(DFTU_SVC_COMPOSE).
 * Include dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_COMPOSE "dftu.svc.compose@1"

/** A leaf op: transform `in_size` bytes at `in` into `out_size` bytes at `out`,
   returning a dftu_task the host drives to completion (NULL = ran inline).
   Writes *rc (0 ok, <0 error). `in`/`out` are POD value buffers, borrowed for
   the await.
 */
typedef dftu_task* (*dftu_op_fn)(void* state, const void* in, void* out,
                                 int* rc);

/** Compose async ops as first-class handles, the value-typed tier over
   dftu_svc_coro's task combinators. An op transforms a POD input value into a
   POD output value; the byte sizes are carried on the handle so `then` can
   thread an intermediate and check out_size(a) == in_size(b). Ops are
   scan-lifetime (freed at fold teardown); free_op is an optional early release.
   Values cross as void*+size, the same erasure the util registry uses. */
typedef struct dftu_svc_compose {
    /** A leaf op wrapping `fn` with owned `state` (freed via free_state at op
       teardown). in_ty/out_ty are the value types (DFTU_T_BYTES for an opaque
       POD); in_size/out_size are their byte sizes, sized so `then` can thread
       an intermediate. */
    dftu_op* (*make_op)(void* h, dftu_op_fn fn, void* state,
                        void (*free_state)(void*), dftu_type in_ty,
                        uint32_t in_size, dftu_type out_ty, uint32_t out_size);
    /** Pipe: run `a`, feed its output as `b`'s input. Requires the piped value
       to match by type and size: out_ty(a)==in_ty(b) and
       out_size(a)==in_size(b); NULL on mismatch. */
    dftu_op* (*then)(void* h, dftu_op* a, dftu_op* b);
    /** Join: run all `n` ops on the same input, output is their outputs
       concatenated in order (out_size = sum of child out_sizes). */
    dftu_op* (*when_all)(void* h, dftu_op* const* ops, uint32_t n);
    /** Race: run all `n` ops on the same input, output is the first to finish
       (all ops must share one out_size). */
    dftu_op* (*when_any)(void* h, dftu_op* const* ops, uint32_t n);
    /** Run `op` over `in`, writing `out` and *rc. `in`/`out` must outlive the
       await; sized by op's in_size/out_size. */
    dftu_task* (*run)(void* h, dftu_op* op, const void* in, void* out, int* rc);
    /** Optional early release; ops are otherwise freed at fold teardown. */
    void (*free_op)(void* h, dftu_op* op);
} dftu_svc_compose;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_COMPOSE_H */
