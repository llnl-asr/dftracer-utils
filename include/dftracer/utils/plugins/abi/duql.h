// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_DUQL_H
#define DFTRACER_UTILS_PLUGINS_ABI_DUQL_H

/** @file
 * dftu.svc.query: the compiled predicate DSL lent to a plugin. Optional
 * service group, fetched via dftu_plugin_host::get_service(DFTU_SVC_DUQL).
 * Include dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_DUQL "dftu.svc.duql"

typedef struct dftu_svc_duql {
    uint32_t struct_size; /**< sizeof(dftu_svc_duql) */
    /** Compile `src`; a parse error is DFTU_E_PARSE with its text in ctx. */
    dftu_status (*duql_compile)(dftu_ctx* ctx, void* h, const char* src,
                                uint32_t len, dftu_duql** out);
    /** Evaluate `q` against row `row` of `df` (a batch's columns, by name). */
    int32_t (*duql_matches)(void* h, const dftu_duql* q,
                            const dftu_dataframe* df, int64_t row);
} dftu_svc_duql;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_DUQL_H */
