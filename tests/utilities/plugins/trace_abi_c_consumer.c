/* A real C consumer of the dftu_svc_trace ABI: open a .pfw.gz, append events,
 * close, then scan it back - all through the raw vtable from C. */
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

/* Each callback delivers one scanned batch as a dftu_dataframe*; sum its rows.
 */
static void count_cb(const void* item, void* ud) {
    const dftu_dataframe* df = (const dftu_dataframe*)item;
    *(int*)ud += (int)dftu_dataframe_num_rows(df);
}

/* Write every row of `df` to `path`, then read it back; *out_read gets the
 * count. */
int dftu_test_trace_roundtrip(const dftu_plugin_host* h, const char* path,
                              const dftu_dataframe* df, int* out_read) {
    const dftu_svc_trace* t =
        (const dftu_svc_trace*)h->get_service(h->h, DFTU_SVC_TRACE);
    if (!t) return -1;
    dftu_trace_writer* w = t->trace_open_write(h->h, path);
    if (!w) return -1;
    int wrote = t->trace_write(h->h, w, df);
    if (t->trace_close(h->h, w) != 0 || wrote != 0) return -1;

    int count = 0;
    if (t->trace_read(h->h, path, count_cb, &count) != 0) return -1;
    *out_read = count;
    return 0;
}
