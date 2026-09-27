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

#define DFTU_SVC_DUQL "dftu.svc.duql@1"

typedef struct dftu_svc_duql {
    dftu_duql* (*duql_compile)(void* h, const char* src, uint32_t len);
    /** Evaluate `q` against row `row` of `df` (a batch's columns, by name). */
    int (*duql_matches)(void* h, const dftu_duql* q, const dftu_dataframe* df,
                        int64_t row);
} dftu_svc_duql;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_DUQL_H */
