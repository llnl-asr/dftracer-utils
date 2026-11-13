// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_AGG_H
#define DFTRACER_UTILS_PLUGINS_ABI_AGG_H

/** @file
 * dftu.svc.agg: the host-owned cross-batch aggregation accumulator lent to a
 * plugin. Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_AGG). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/dataframe/agg_op_codes.h> /* DFTU_AGG_* for dftu_agg_col */
#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/result.h>     /* dftu_result_value */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_AGG "dftu.svc.agg"

/** Host-owned cross-batch aggregation accumulator. This is the ONE
   accumulator a plugin gets: it wraps the dataframe engine's mergeable
   AggState, so a keyed map is an accumulator with key columns, a scalar
   handle is one with zero key columns, and the reduction vocabulary is the
   engine's agg op table. The host merges same-named accumulators across
   worker slices and finalizes each at scan end to a native dataframe (the key
   columns, then one column per aggregate), returned to run() under `name`. */
typedef struct dftu_agg dftu_agg;

/** One aggregate for a dftu_svc_agg accumulator. `op` is a DFTU_AGG_* code (the
   dftu_agg_op enum, kept a fixed-width int at the seam for ABI stability); a
   code outside the DFTU_AGG_* range makes agg_new fail. `value` names
   the value column in each accumulated batch dataframe (NULL for
   DFTU_AGG_COUNT, the group row count); `out` names the result column; `param`
   is the op's scalar parameter (a quantile in [0,1] for DFTU_AGG_PCT, k for
   DFTU_AGG_TOPK and friends, 0 otherwise); `by` names the op's second input
   column (the ordering column for DFTU_AGG_ARGMAX/ARGMIN/TOPK, the dur column
   for the occupancy ops, x for the co-moment ops; NULL when the op takes one
   input). All names are borrowed for the agg_new call only. */
typedef struct dftu_agg_col {
    int32_t op;
    const char* value;
    const char* out;
    double param;
    const char* by;
} dftu_agg_col;

/** A plugin-defined mergeable state type, registered from the factory. dftu_agg
   covers every shape the engine's agg table knows; this covers the ones it does
   not - a call tree, an interval tree, an adjacency list, a custom sketch. The
   host drives a registered state exactly as it drives an accumulator, without
   knowing its shape: one init per worker slice, update per batch, merge at the
   fan-in, serialize to spill when the slice runs past the scan's memory budget,
   finalize once on the merged state, destroy for every state it created.

   Each callback takes the host's @ref dftu_ctx first and returns a
   @ref dftu_status; a negative status fails the scan with the message the
   plugin set through @ref dftu_fail. */
typedef struct dftu_state_desc {
    uint32_t struct_size; /**< sizeof(dftu_state_desc) */
    /** Registry key; must be `<plugin>.<name>` and outlive the registration.
       The host emits the finalized result under this name. */
    const char* name;
    /** A fresh empty state into `*out_state`; `self` is the pointer given to
       register_state. */
    dftu_status (*init)(dftu_ctx* ctx, void* self, void** out_state);
    /** Fold one batch in. `batch` is host-owned and borrowed for the call. */
    dftu_status (*update)(dftu_ctx* ctx, void* state,
                          const dftu_dataframe* batch);
    /** Fold `other` into `into`; must be associative. `other` stays valid and
       is destroyed by the host. */
    dftu_status (*merge)(dftu_ctx* ctx, void* into, void* other);
    /** Live size in bytes. The host's ONLY measure of the state against the
       scan's memory budget; NULL means unmeasurable, and the host then never
       spills the state however large it grows. */
    uint64_t (*bytes)(const void* state);
    /** Optional pair, given together or not at all: enables spill under budget
       and distributed partials. NULL = in-memory only, and a state past the
       budget is REFUSED a spill with an error rather than silently kept.
       serialize fills `out`; the host releases it through out->free_fn.
       deserialize returns a new state the host owns. */
    dftu_status (*serialize)(dftu_ctx* ctx, const void* state, dftu_bytes* out);
    dftu_status (*deserialize)(dftu_ctx* ctx, void* self, dftu_bytes in,
                               void** out_state);
    /** The whole-scan result, filled into `out` (see dftu_svc_result::emit for
       ownership per kind). Called once, on the merged state. */
    dftu_status (*finalize)(dftu_ctx* ctx, void* state, dftu_result_value* out);
    /** Release a state from init or deserialize; one call per state. */
    void (*destroy)(void* state);
} dftu_state_desc;

typedef struct dftu_svc_agg {
    uint32_t struct_size; /**< sizeof(dftu_svc_agg) */
    /** Get-or-create a named accumulator grouping by the `key_n` columns named
       in `key_names` and computing each of `spec_n` aggregates. `*out` is a
       stable handle owned by the host (freed at fold teardown, never by the
       plugin). Fails with DFTU_E_INVALID on a bad op code, a missing output
       name, or a `name` in the host's own "dftu." namespace, which is refused
       to plugins. A name seen before returns the existing handle and ignores
       the new spec. Safe from any slice thread on that slice's host. */
    dftu_status (*agg_new)(dftu_ctx* ctx, void* h, const char* name,
                           const char* const* key_names, uint32_t key_n,
                           const dftu_agg_col* specs, uint32_t spec_n,
                           dftu_agg** out);
    /** Fold one batch into `a`: each key and value column is looked up by name
       in `df` (host-owned, borrowed for the call) and accumulated. A batch
       missing any referenced column is skipped. Serial per accumulator; one
       slice's accumulator is touched by one thread. */
    dftu_status (*agg_accumulate)(dftu_ctx* ctx, void* h, dftu_agg* a,
                                  const dftu_dataframe* df);
    /** The cross-worker-merged, finalized result of the accumulator named
       `name` - any plugin's, which is how one plugin reads another's whole-scan
       aggregate. Call at on_finalize; the host finalizes the producer first,
       from the plugins' declared dftu_plugin::provides / dftu_plugin::consumes
       rather than registration order. Returns a NEW owned dataframe the caller
       frees with dftu_dataframe_free. DFTU_OK with `*out` NULL says the
       producer created no such accumulator (an empty scan). */
    dftu_status (*agg_result)(dftu_ctx* ctx, void* h, const char* name,
                              dftu_dataframe** out);
    /** Register a mergeable state type; `self` is handed back to init and
       deserialize and must outlive the plugin. Callable ONLY from the factory:
       by the time a fold runs, the registry the host plans from is settled, so
       the run-time host refuses it. The descriptor is COPIED, so it need not
       outlive the call. Fails with DFTU_E_INVALID on a NULL desc, a name
       that is NULL, in the host's "dftu." namespace, not `<plugin>.<name>`, or
       already registered, a missing init/update/merge/finalize/destroy, or a
       serialize/deserialize pair given only half. */
    dftu_status (*register_state)(dftu_ctx* ctx, void* h,
                                  const dftu_state_desc* desc, void* self);
} dftu_svc_agg;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_AGG_H */
