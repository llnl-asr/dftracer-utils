// SPDX-License-Identifier: MIT
/* Test-only plugin: a second file format that scores the same 90 on the TOY1
 * magic toy_format_plugin.c claims, so loading both makes every scan of such a
 * file ambiguous. It never opens anything: the engine must refuse the tie
 * before asking either format to. */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <string.h>

static int32_t tie_probe(void* unused_self, const uint8_t* head, uint64_t n,
                         const char* unused_path) {
    static const char magic[] = "TOY1";
    (void)unused_self;
    (void)unused_path;
    if (n >= sizeof(magic) - 1 && memcmp(head, magic, sizeof(magic) - 1) == 0)
        return 90;
    return 0;
}

static dftu_status tie_open(dftu_ctx* ctx, void* unused_self, const char* path,
                            void** file) {
    (void)unused_self;
    (void)path;
    (void)file;
    return dftu_fail(ctx, DFTU_E_INTERNAL,
                     "toy_format_tie: it was asked to open a file");
}

static dftu_status tie_schema(dftu_ctx* ctx, void* unused_file,
                              dftu_schema* out) {
    (void)unused_file;
    (void)out;
    return dftu_fail(ctx, DFTU_E_INTERNAL,
                     "toy_format_tie: it was asked to declare a schema");
}

static dftu_status tie_chunks(dftu_ctx* ctx, void* unused_file,
                              uint64_t* count) {
    (void)unused_file;
    *count = 1;
    return dftu_fail(ctx, DFTU_E_INTERNAL,
                     "toy_format_tie: it was asked for its chunks");
}

static dftu_status tie_scan(dftu_ctx* ctx, void* unused_file, uint64_t chunk,
                            const dftu_plugin_host* unused_host,
                            const dftu_scan_request* req, int32_t* out_pushed,
                            void** cursor_self,
                            const dftu_cursor_vt** cursor_vt) {
    (void)unused_file;
    (void)unused_host;
    (void)chunk;
    (void)req;
    (void)out_pushed;
    (void)cursor_self;
    (void)cursor_vt;
    return dftu_fail(ctx, DFTU_E_INTERNAL,
                     "toy_format_tie: it was asked to scan a chunk");
}

static void tie_close(void* file) { (void)file; }
static void tie_destroy(void* self) { (void)self; }

static const dftu_file_format_vt TIE_FORMAT_VT = {
    .struct_size = sizeof(dftu_file_format_vt),
    .extensions = NULL,
    .probe = tie_probe,
    .open = tie_open,
    .schema = tie_schema,
    .chunks = tie_chunks,
    .chunk_stats = NULL,
    .scan = tie_scan,
    .close = tie_close,
    .destroy = tie_destroy};

static dftu_plugin plugin;

static dftu_status make_slice(dftu_ctx* unused_ctx, void* unused_self,
                              void** slice) {
    (void)unused_ctx;
    (void)unused_self;
    *slice = NULL;
    return DFTU_OK;
}
static dftu_status on_batch(dftu_ctx* unused_ctx, void* unused_self,
                            const dftu_dataframe* unused_batch,
                            const dftu_plugin_host* unused_host) {
    (void)unused_ctx;
    (void)unused_self;
    (void)unused_batch;
    (void)unused_host;
    return DFTU_OK;
}
static dftu_status merge(dftu_ctx* unused_ctx, void* unused_a, void* unused_b) {
    (void)unused_ctx;
    (void)unused_a;
    (void)unused_b;
    return DFTU_OK;
}
static void destroy_slice(void* unused_self) { (void)unused_self; }
static void destroy(void* unused_self) { (void)unused_self; }

DFTU_PLUGIN_INFO("toy_format_tie", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* unused_config,
                                               dftu_plugin** out) {
    (void)unused_config;
    const dftu_svc_file_formats* formats =
        (const dftu_svc_file_formats*)h->get_service(h->h,
                                                     DFTU_SVC_FILE_FORMATS);
    if (!formats || !formats->register_format)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no file format registry");
    const dftu_status registered = formats->register_format(
        ctx, h->h, "toy_format_tie.rows", &TIE_FORMAT_VT, NULL);
    if (registered < 0) return registered;

    memset(&plugin, 0, sizeof(plugin));
    plugin.struct_size = sizeof(plugin);
    plugin.make_slice = make_slice;
    plugin.on_batch = on_batch;
    plugin.merge = merge;
    plugin.destroy_slice = destroy_slice;
    plugin.destroy = destroy;
    *out = &plugin;
    return DFTU_OK;
}
