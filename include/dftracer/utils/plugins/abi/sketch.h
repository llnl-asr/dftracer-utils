#ifndef DFTRACER_UTILS_PLUGINS_ABI_SKETCH_H
#define DFTRACER_UTILS_PLUGINS_ABI_SKETCH_H

/** @file
 * dftu.svc.sketch: a mergeable quantile accumulator lent to a plugin.
 * Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_SKETCH). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_SKETCH "dftu.svc.sketch@1"

typedef struct {
    uint64_t count;
    double min, max, mean, p50, p90, p95, p99;
} dftu_quantiles;

typedef struct dftu_svc_sketch {
    /** Mergeable quantile accumulator; the plugin must sketch_free what it
       creates. add's w is the sample weight. */
    dftu_sketch* (*sketch_create)(void* h);
    void (*sketch_add)(void* h, dftu_sketch* s, double v, double w);
    void (*sketch_merge)(void* h, dftu_sketch* into, const dftu_sketch* other);
    dftu_quantiles (*sketch_result)(void* h, const dftu_sketch* s);
    void (*sketch_free)(void* h, dftu_sketch* s);
} dftu_svc_sketch;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_SKETCH_H */
