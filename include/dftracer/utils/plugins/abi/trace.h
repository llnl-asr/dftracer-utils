// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_TRACE_H
#define DFTRACER_UTILS_PLUGINS_ABI_TRACE_H

/** @file
 * dftu.svc.trace: dftracer trace file I/O lent to a plugin. Optional service
 * group, fetched via dftu_plugin_host::get_service(DFTU_SVC_TRACE). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_TRACE "dftu.svc.trace"

typedef struct dftu_svc_trace {
    uint32_t struct_size; /**< sizeof(dftu_svc_trace) */
    /** dftracer trace writer: open a gzip .pfw.gz, append events, then close.
       The index is built lazily on first read, not at close. */
    dftu_status (*trace_open_write)(dftu_ctx* ctx, void* h, const char* path,
                                    dftu_trace_writer** out);
    /** Append every row of `df` (a batch's columns, by name) as a trace event.
     */
    dftu_status (*trace_write)(dftu_ctx* ctx, void* h, dftu_trace_writer* w,
                               const dftu_dataframe* df);
    dftu_status (*trace_close)(dftu_ctx* ctx, void* h, dftu_trace_writer* w);
    /** Scan a trace file (auto-indexed) and call on_batch once per scanned
       batch with a dftu_dataframe* (item), ids interned into the host scan
       table; the frame is valid only for the call. A status other than
       DFTU_OK from on_batch (an error, or DFTU_END) stops the delivery of
       batches and is returned. */
    dftu_status (*trace_read)(dftu_ctx* ctx, void* h, const char* path,
                              dftu_stream_item_fn on_batch, void* ud);
} dftu_svc_trace;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_TRACE_H */
