#ifndef DFTRACER_UTILS_PLUGINS_ABI_QUERY_H
#define DFTRACER_UTILS_PLUGINS_ABI_QUERY_H

/** @file
 * dftu.svc.query: the compiled predicate DSL lent to a plugin. Optional
 * service group, fetched via dftu_plugin_host::get_service(DFTU_SVC_QUERY).
 * Include dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_QUERY "dftu.svc.query@1"

typedef struct dftu_svc_query {
    dftu_query* (*query_compile)(void* h, const char* src, uint32_t len);
    /** Evaluate `q` against row `row` of `df` (a batch's columns, by name). */
    int (*query_matches)(void* h, const dftu_query* q, const dftu_dataframe* df,
                         int64_t row);
} dftu_svc_query;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_QUERY_H */
