// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_OPS_H
#define DFTRACER_UTILS_PLUGINS_ABI_OPS_H

/** @file
 * dftu.svc.ops: the dataframe engine's op registry lent to a plugin by name.
 * Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_OPS). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/dataframe/c_api.h> /* dftu_scalar, by value below */
#include <dftracer/utils/dataframe/c_api_udaf.h> /* dftu_udaf_desc */
#include <dftracer/utils/dataframe/c_api_udf.h>  /* dftu_udf_desc */
#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_OPS "dftu.svc.ops"

/** Host-service group exposing the dataframe engine's op registry. Each slot
   is a thin name-keyed forwarder to the matching dftu_op_run / dftu_op_find /
   dftu_op_register function (dftracer/utils/dataframe/c_api.h): a plugin looks
   an op up by name and runs it on Series/DataFrame handles it already holds,
   with no need to link the dataframe C ABI itself. Every call that can fail
   returns a @ref dftu_status and sets the message on the caller's ctx. */
typedef struct dftu_svc_ops {
    uint32_t struct_size; /**< sizeof(dftu_svc_ops) */
    /** find(name) then dftu_op_run: `in` are `n_in` borrowed input columns,
       `args` supplies the op's other operands (NULL if none). On success
       `*out` is a new owned column (free with dftu_series_free). Fails with
       DFTU_E_NOT_FOUND for an unknown `name`, DFTU_E_INVALID when the call
       mismatches the op's kind/arity/shape. */
    dftu_status (*run)(dftu_ctx* ctx, void* h, const char* name,
                       const dftu_series* const* in, uint32_t n_in,
                       const dftu_op_arg* args, dftu_series** out);
    /** find(name) then dftu_op_run_aggregate on one column: `in[0]` is the
       reduced column (n_in must be 1). On success `*out` holds the reduction;
       failures as in `run`. */
    dftu_status (*run_aggregate)(dftu_ctx* ctx, void* h, const char* name,
                                 const dftu_series* const* in, uint32_t n_in,
                                 const dftu_op_arg* args, dftu_scalar* out);
    /** find(name) then dftu_op_run_frame: `frames` are `n_in` borrowed input
       dataframes. On success `*out` is a new owned dataframe (free with
       dftu_dataframe_free); failures as in `run`. */
    dftu_status (*run_frame)(dftu_ctx* ctx, void* h, const char* name,
                             const dftu_dataframe* const* frames, uint32_t n_in,
                             const dftu_op_arg* args, dftu_dataframe** out);
    /** The registered op named `name` (built-in or user), or NULL if none. See
       dftu_op_find. */
    const dftu_op_desc* (*find)(void* h, const char* name);
    /** Register a user op; see dftu_op_register. A plugin op must be named
       `<plugin>.<name>`: the bare namespace holds the host's built-in ops
       (`add`, `sum`, ...) and the "dftu." prefix its internal ones, so both
       belong to the host and are refused here, and a host op can never be
       shadowed. Fails with DFTU_E_INVALID if `desc`/its name is NULL, the
       name is already registered, or the name is one the host keeps. */
    dftu_status (*register_op)(dftu_ctx* ctx, void* h,
                               const dftu_op_desc* desc);
    /** find(name) then dftu_op_run_lazy: `in` are `n_in` borrowed input
       lazyframes. On success `*out` is a new owned lazyframe (free with
       dftu_lazyframe_free); failures as in `run`. */
    dftu_status (*run_lazy)(dftu_ctx* ctx, void* h, const char* name,
                            const dftu_lazyframe* const* in, uint32_t n_in,
                            const dftu_op_arg* args, dftu_lazyframe** out);
    /** Register a scalar UDF that duql calls by `desc->name`; see
       dftu_udf_desc. `self` is passed to its bind. The host copies `desc`, and
       the plugin library stays loaded while a plan holds the call. Fails with
       DFTU_E_INVALID, naming the problem on `ctx`, if `desc` or its name is
       NULL, the name is not a duql function name, it is a built-in function or
       another UDF, a flag is unknown, or the argument counts do not fit; with
       DFTU_E_UNSUPPORTED after the load. Registration is a load-time call. */
    dftu_status (*register_udf)(dftu_ctx* ctx, void* h,
                                const dftu_udf_desc* desc, void* self);
    /** Register a user-defined aggregate under `desc->name`; see
       dftu_udaf_desc. `self` is passed to its bind. The host copies `desc`. The
       UDAF is registered; no query can call it yet. Fails with
       DFTU_E_INVALID, naming the problem on `ctx`, if `desc` or its name is
       NULL, the name is not a duql function name, it is a built-in function or
       aggregate, an op, a UDF or another UDAF, a flag is unknown, a required
       callback is missing, `state_size` is 0, `state_align` is not a power of
       two up to 64, the argument counts do not fit, or the state is not
       DFTU_AGG_STATE_POD and has no destroy, serialize and deserialize; with
       DFTU_E_UNSUPPORTED after the load. Registration is a load-time call. */
    dftu_status (*register_udaf)(dftu_ctx* ctx, void* h,
                                 const dftu_udaf_desc* desc, void* self);
} dftu_svc_ops;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_OPS_H */
