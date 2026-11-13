// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_ARROW_H
#define DFTRACER_UTILS_PLUGINS_ABI_ARROW_H

/** @file
 * dftu.svc.arrow: Arrow IPC file I/O lent to a plugin. Optional service
 * group, fetched via dftu_plugin_host::get_service(DFTU_SVC_ARROW). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_ARROW "dftu.svc.arrow"

typedef struct dftu_svc_arrow {
    uint32_t struct_size; /**< sizeof(dftu_svc_arrow) */
    /** Write a plugin-provided Arrow batch to an IPC file. */
    dftu_status (*arrow_write_ipc)(dftu_ctx* ctx, void* h, struct ArrowArray* a,
                                   struct ArrowSchema* s, const char* path);
    /** Read the first record batch of an Arrow IPC file; the plugin owns *out
       and *out_schema and must call their release. */
    dftu_status (*arrow_read_ipc)(dftu_ctx* ctx, void* h, const char* path,
                                  struct ArrowArray* out,
                                  struct ArrowSchema* out_schema);
} dftu_svc_arrow;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_ARROW_H */
