// SPDX-License-Identifier: MIT
/* Test-only plugin that leaks. It registers one file format under the bare
 * name "leaky" (the plugin's own name, which the namespace rule allows) whose
 * scan takes a block from the host's memory service and never gives it back,
 * so the memory exit check reports the leak when the plugin unloads.
 *
 * `dftracer_plugin check --tests tests` on this library must fail. The golden
 * test next to it reads a `.leaky` file, which is what calls scan(). */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <string.h>

#define LEAKY_BYTES 4096u

static dftu_plugin plugin;

static int32_t leaky_probe(void* self, const uint8_t* head, uint64_t n,
                           const char* path) {
    (void)self;
    (void)head;
    (void)n;
    const size_t len = path ? strlen(path) : 0;
    if (len >= 6 && strcmp(path + len - 6, ".leaky") == 0) return 100;
    return 0;
}

static dftu_status leaky_open(dftu_ctx* ctx, void* self, const char* path,
                              void** file) {
    (void)self;
    (void)path;
    (void)ctx;
    *file = (void*)&plugin;
    return DFTU_OK;
}

static dftu_status leaky_schema(dftu_ctx* ctx, void* file, dftu_schema* out) {
    (void)ctx;
    (void)file;
    return dftu_schema_add_field(out, "id", DFTU_TYPE_INT64, 0,
                                 DFTU_TIME_UNIT_MICRO, NULL, 0, 0, 0);
}

static dftu_status leaky_chunks(dftu_ctx* ctx, void* file, uint64_t* count) {
    (void)ctx;
    (void)file;
    *count = 1;
    return DFTU_OK;
}

typedef struct {
    int32_t emitted;
} LeakyCursor;

static LeakyCursor g_cursor;

static dftu_status leaky_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                              dftu_dataframe** out, dftu_task** pending) {
    (void)self;
    (void)max_rows;
    (void)pending;
    *out = NULL;
    if (g_cursor.emitted) return DFTU_END;
    g_cursor.emitted = 1;
    const int64_t one = 1;
    dftu_series* column = dftu_series_new_flat(DFTU_TYPE_INT64, &one, 1, NULL);
    if (!column) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "leaky: no column");
    const char* names[] = {"id"};
    dftu_series* columns[] = {column};
    *out = dftu_dataframe_new(names, columns, 1);
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "leaky: no dataframe");
    return DFTU_OK;
}

static void leaky_cursor_destroy(void* self) { (void)self; }

static const dftu_cursor_vt LEAKY_CURSOR = {sizeof(dftu_cursor_vt), leaky_next,
                                            leaky_cursor_destroy, NULL};

static dftu_status leaky_scan(dftu_ctx* ctx, void* file, uint64_t chunk,
                              const dftu_plugin_host* host,
                              const dftu_scan_request* req, int32_t* pushed,
                              void** cursor_self,
                              const dftu_cursor_vt** cursor_vt) {
    (void)file;
    (void)chunk;
    (void)req;
    (void)pushed;
    const dftu_svc_memory* memory =
        host ? (const dftu_svc_memory*)host->get_service(host->h,
                                                         DFTU_SVC_MEMORY)
             : NULL;
    if (!memory || !memory->alloc)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "leaky: no memory service");
    /* Never freed: the exit check reports this block after the unload. */
    void* block = memory->alloc(host->h, LEAKY_BYTES, 8);
    if (!block) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "leaky: no block");
    g_cursor.emitted = 0;
    *cursor_self = (void*)&plugin;
    *cursor_vt = &LEAKY_CURSOR;
    return DFTU_OK;
}

static void leaky_close(void* file) { (void)file; }

static const dftu_file_format_vt LEAKY_FORMAT = {sizeof(dftu_file_format_vt),
                                                 NULL,
                                                 leaky_probe,
                                                 leaky_open,
                                                 leaky_schema,
                                                 leaky_chunks,
                                                 NULL,
                                                 leaky_scan,
                                                 leaky_close,
                                                 NULL,
                                                 NULL};

DFTU_PLUGIN_INFO("leaky", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)config;
    const dftu_svc_file_formats* formats =
        (const dftu_svc_file_formats*)h->get_service(h->h,
                                                     DFTU_SVC_FILE_FORMATS);
    if (!formats || !formats->register_format)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no file format registry");
    memset(&plugin, 0, sizeof(plugin));
    plugin.struct_size = sizeof(plugin);
    *out = &plugin;
    return formats->register_format(ctx, h->h, "leaky", &LEAKY_FORMAT, NULL);
}
