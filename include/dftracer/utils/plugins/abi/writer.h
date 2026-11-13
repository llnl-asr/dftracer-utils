// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_WRITER_H
#define DFTRACER_UTILS_PLUGINS_ABI_WRITER_H

/** @file
 * dftu.svc.writer: a host-owned parallel output writer lent to a plugin.
 * Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_WRITER). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_WRITER "dftu.svc.writer"

typedef struct dftu_svc_writer {
    uint32_t struct_size; /**< sizeof(dftu_svc_writer) */
    /** The task calls return in `*out`; the host awaits it. */
    dftu_status (*merge_shards)(dftu_ctx* ctx, void* h, const char* target,
                                const char* const* shards, uint32_t n,
                                dftu_task** out);
    dftu_status (*writer_create)(dftu_ctx* ctx, void* h, const char* path,
                                 uint32_t num_workers, int32_t gzip,
                                 dftu_writer** out);
    dftu_status (*writer_open)(dftu_ctx* ctx, void* h, dftu_writer* w,
                               dftu_task** out);
    dftu_status (*writer_chunk)(dftu_ctx* ctx, void* h, dftu_writer* w,
                                uint32_t worker, const void* data, uint64_t len,
                                dftu_task** out);
    dftu_status (*writer_close)(dftu_ctx* ctx, void* h, dftu_writer* w,
                                dftu_task** out);
} dftu_svc_writer;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_WRITER_H */
