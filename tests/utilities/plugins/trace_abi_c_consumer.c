// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_trace ABI: open a .pfw.gz, append events,
 * close, then scan it back - all through the raw vtable from C. */
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <utilities/plugins/abi_c_consumers.h>

/* Each callback delivers one scanned batch as a dftu_dataframe*; sum its rows.
 */
static dftu_status count_cb(dftu_ctx* ctx, const void* item, void* ud) {
    const dftu_dataframe* df = (const dftu_dataframe*)item;
    (void)ctx;
    *(int*)ud += (int)dftu_dataframe_num_rows(df);
    return DFTU_OK;
}

/* Write every row of `df` to `path`, then read it back; *out_read gets the
 * count. */
int dftu_test_trace_roundtrip(dftu_ctx* ctx, const dftu_plugin_host* h,
                              const char* path, const dftu_dataframe* df,
                              int* out_read) {
    const dftu_svc_trace* t =
        (const dftu_svc_trace*)h->get_service(h->h, DFTU_SVC_TRACE);
    dftu_trace_writer* w = NULL;
    dftu_status wrote;
    int count = 0;
    if (!t) return -1;
    if (t->trace_open_write(ctx, h->h, path, &w) < 0) return -1;
    wrote = t->trace_write(ctx, h->h, w, df);
    if (t->trace_close(ctx, h->h, w) < 0 || wrote < 0) return -1;
    if (t->trace_read(ctx, h->h, path, count_cb, &count) < 0) return -1;
    *out_read = count;
    return 0;
}
