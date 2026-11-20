// SPDX-License-Identifier: MIT
/* Test-only plugin that crashes. It registers one file format under the bare
 * name "crash" (the plugin's own name) whose scan() calls abort(), so
 * `dftracer_plugin check` on a golden test over a `.crash` file reports the
 * child's death by signal and exits non-zero, while the tool itself returns.
 *
 * The abort is inside a plugin callback, and a probe never runs one, so the
 * library loads cleanly first: only the query runs the aborting callback. */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static dftu_plugin plugin;

static int32_t crash_probe(void* self, const uint8_t* head, uint64_t n,
                           const char* path) {
    (void)self;
    (void)head;
    (void)n;
    const size_t len = path ? strlen(path) : 0;
    if (len >= 6 && strcmp(path + len - 6, ".crash") == 0) return 100;
    return 0;
}

static dftu_status crash_open(dftu_ctx* ctx, void* self, const char* path,
                              void** file) {
    (void)ctx;
    (void)self;
    (void)path;
    *file = (void*)&plugin;
    return DFTU_OK;
}

static dftu_status crash_schema(dftu_ctx* ctx, void* file, dftu_schema* out) {
    (void)ctx;
    (void)file;
    return dftu_schema_add_field(out, "id", DFTU_TYPE_INT64, 0,
                                 DFTU_TIME_UNIT_MICRO, NULL, 0, 0, 0);
}

static dftu_status crash_chunks(dftu_ctx* ctx, void* file, uint64_t* count) {
    (void)ctx;
    (void)file;
    *count = 1;
    return DFTU_OK;
}

static dftu_status crash_scan(dftu_ctx* ctx, void* file, uint64_t chunk,
                              const dftu_plugin_host* host,
                              const dftu_scan_request* req, int32_t* pushed,
                              void** cursor_self,
                              const dftu_cursor_vt** cursor_vt) {
    (void)ctx;
    (void)file;
    (void)chunk;
    (void)host;
    (void)req;
    (void)pushed;
    (void)cursor_self;
    (void)cursor_vt;
    abort();
}

static void crash_close(void* file) { (void)file; }

static const dftu_file_format_vt CRASH_FORMAT = {sizeof(dftu_file_format_vt),
                                                 NULL,
                                                 crash_probe,
                                                 crash_open,
                                                 crash_schema,
                                                 crash_chunks,
                                                 NULL,
                                                 crash_scan,
                                                 crash_close,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 NULL};

DFTU_PLUGIN_INFO("crash", "1.0")

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
    return formats->register_format(ctx, h->h, "crash", &CRASH_FORMAT, NULL);
}
