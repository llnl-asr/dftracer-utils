#ifndef DFTRACER_UTILS_PLUGINS_ABI_RESULT_H
#define DFTRACER_UTILS_PLUGINS_ABI_RESULT_H

/** @file
 * dftu.svc.result: the named result channel lent to a plugin. Optional
 * service group, fetched via dftu_plugin_host::get_service(DFTU_SVC_RESULT).
 * Include dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_RESULT "dftu.svc.result@1"

/** The alternative a dftu_result_value carries; matches the host's own
   NamedResult variant one for one. */
typedef enum {
    DFTU_RESULT_KIND_BYTES = 0,
    DFTU_RESULT_KIND_ARROW,
    DFTU_RESULT_KIND_FRAME,
    DFTU_RESULT_KIND_LAZYFRAME
} dftu_result_kind;

/** A tagged named-result value: exactly one of `bytes`/`arrow`/`frame`/
   `lazyframe` is live, selected by `kind`. See dftu_svc_result::emit for
   ownership per kind. */
typedef struct dftu_result_value {
    int32_t kind; /**< a dftu_result_kind value */
    union {
        struct {
            const void* data;
            uint64_t len;
        } bytes;
        struct {
            struct ArrowArray* array;
            struct ArrowSchema* schema;
        } arrow;
        dftu_dataframe* frame;
        dftu_lazyframe* lazyframe;
    } u;
} dftu_result_value;

/** Named result channel. The host moves Arrow/frame/lazyframe handles and
   copies bytes, never interpreting any of them; Plugins::run returns the
   collected results to the caller keyed by name. */
typedef struct dftu_svc_result {
    /** Emit a named result carrying `v`. DFTU_RESULT_KIND_BYTES is COPIED
       (`v->u.bytes.len` bytes); ARROW/FRAME/LAZYFRAME are MOVED - the host
       takes ownership of the array/schema or handle (do not use or free it
       after a successful call). A LAZYFRAME plan MUST be self-contained - its
       source an in-memory frame or a re-openable source (dftu_dataframe_lazy
       of a materialized frame is the supported construction); a plan
       referencing the plugin's per-scan/executor state, which dies at
       finalize, is a plugin bug. Intended for on_finalize (called on the
       merged master fold); if called concurrently from on_batch the host
       serializes writes, last-writer-wins on a name. Returns 0 ok, -1 on a
       NULL name/v or an unrecognized kind. */
    int (*emit)(void* h, const char* name, dftu_result_value* v);
} dftu_svc_result;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_RESULT_H */
