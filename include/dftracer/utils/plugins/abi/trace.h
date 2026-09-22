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

#define DFTU_SVC_TRACE "dftu.svc.trace@1"

typedef struct dftu_svc_trace {
    /** dftracer trace writer: open a gzip .pfw.gz, append events, then close.
       The index is built lazily on first read, not at close. */
    dftu_trace_writer* (*trace_open_write)(void* h, const char* path);
    /** Append every row of `df` (a batch's columns, by name) as a trace event.
     */
    int (*trace_write)(void* h, dftu_trace_writer* w, const dftu_dataframe* df);
    int (*trace_close)(void* h, dftu_trace_writer* w);
    /** Scan a trace file (auto-indexed) and call on_batch once per scanned
       batch with a dftu_dataframe* (item), ids interned into the host scan
       table; the frame is valid only for the call. 0 on success. */
    int (*trace_read)(void* h, const char* path, dftu_stream_item_fn on_batch,
                      void* ud);
} dftu_svc_trace;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_TRACE_H */
