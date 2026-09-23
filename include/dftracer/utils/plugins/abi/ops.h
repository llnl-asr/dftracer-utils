#ifndef DFTRACER_UTILS_PLUGINS_ABI_OPS_H
#define DFTRACER_UTILS_PLUGINS_ABI_OPS_H

/** @file
 * dftu.svc.ops: the dataframe engine's op registry lent to a plugin by name.
 * Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_OPS). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/dataframe/abi.h> /* dftu_scalar, by value below */
#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A reduction, or the error that prevented it. Declared here rather than in
   core.h because it holds a dftu_scalar BY VALUE, and core.h keeps every
   dataframe type opaque so a plugin that never touches the engine pays
   nothing for it. */
DFTU_RESULT_DECL(dftu_result_scalar, dftu_scalar);

#ifdef __cplusplus
#endif

#define DFTU_SVC_OPS "dftu.svc.ops@1"

/** Host-service group exposing the dataframe engine's op registry. Each slot
   is a thin name-keyed forwarder to the matching dftu_op_run / dftu_op_find /
   dftu_op_register function (dftracer/utils/dataframe/abi.h): a plugin looks
   an op up by name and runs it on Series/DataFrame handles it already holds,
   with no need to link the dataframe C ABI itself. */
typedef struct dftu_svc_ops {
    /** find(name) then dftu_op_run: `in` are `n_in` borrowed input columns,
       `args` supplies the op's other operands (NULL if none). On success
       carries a new owned column (free with dftu_series_free); on failure
       carries a dftu_error - DFTU_COND_NOT_FOUND for an unknown `name`,
       DFTU_COND_INVALID_ARGUMENT when the call mismatches the op's
       kind/arity/shape. */
    DFTU_RESULT_MUST_CHECK dftu_result_series (*run)(
        void* h, const char* name, const dftu_series* const* in, uint32_t n_in,
        const dftu_op_arg* args);
    /** find(name) then dftu_op_run_aggregate on one column: `in[0]` is the
       reduced column (n_in must be 1). On success carries the reduction; on
       failure carries a dftu_error - see `run`. */
    DFTU_RESULT_MUST_CHECK dftu_result_scalar (*run_aggregate)(
        void* h, const char* name, const dftu_series* const* in, uint32_t n_in,
        const dftu_op_arg* args);
    /** find(name) then dftu_op_run_frame: `in` are `n_in` borrowed input
       dataframes. On success carries a new owned dataframe (free with
       dftu_dataframe_free); on failure carries a dftu_error - see `run`. */
    DFTU_RESULT_MUST_CHECK dftu_result_frame (*run_frame)(
        void* h, const char* name, const dftu_dataframe* const* in,
        uint32_t n_in, const dftu_op_arg* args);
    /** The registered op named `name` (built-in or user), or NULL if none. See
       dftu_op_find. */
    const dftu_op_desc* (*find)(void* h, const char* name);
    /** Register a user op; see dftu_op_register. A plugin op must be named
       `<plugin>.<name>`: the bare namespace holds the host's built-in ops
       (`add`, `sum`, ...) and the "dftu." prefix its internal ones, so both
       belong to the host and are refused here, and a host op can never be
       shadowed. Returns 0 on success, non-zero if `desc`/its name is NULL, the
       name is already registered, or the name is one the host keeps. */
    int (*register_op)(void* h, const dftu_op_desc* desc);
    /** find(name) then dftu_op_run_lazy: `in` are `n_in` borrowed input
       lazyframes. On success carries a new owned lazyframe (free with
       dftu_lazyframe_free); on failure carries a dftu_error - see `run`. */
    DFTU_RESULT_MUST_CHECK dftu_result_lazyframe (*run_lazy)(
        void* h, const char* name, const dftu_lazyframe* const* in,
        uint32_t n_in, const dftu_op_arg* args);
} dftu_svc_ops;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_OPS_H */
