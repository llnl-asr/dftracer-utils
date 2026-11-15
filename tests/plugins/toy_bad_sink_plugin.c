// SPDX-License-Identifier: MIT
/* Test-only plugin: it registers a sink whose `write` callback is NULL, which
 * the host must refuse (a sink needs bind, write and finalize). The factory
 * returns that refusal as it is, so the plugin fails to load with the host's
 * message, which names the sink and the missing callback. */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <string.h>

static dftu_plugin plugin;

static dftu_status s_bind(dftu_ctx* ctx, void* self, const char* const* names,
                          const int32_t* types, uint32_t n_columns,
                          const dftu_value* options, void** sink) {
    (void)ctx;
    (void)self;
    (void)names;
    (void)types;
    (void)n_columns;
    (void)options;
    (void)sink;
    return DFTU_E_INTERNAL;
}

static dftu_status s_finalize(dftu_ctx* ctx, void* sink) {
    (void)ctx;
    (void)sink;
    return DFTU_OK;
}

static void s_abort(void* sink) { (void)sink; }
static void s_destroy(void* self) { (void)self; }

static const dftu_sink_vt BAD_SINK_VT = {.struct_size = sizeof(dftu_sink_vt),
                                         .bind = s_bind,
                                         .write = NULL,
                                         .finalize = s_finalize,
                                         .abort = s_abort,
                                         .destroy = s_destroy};

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

DFTU_PLUGIN_INFO("toy_bad_sink", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* unused_config,
                                               dftu_plugin** out) {
    (void)unused_config;
    const dftu_svc_file_formats* formats =
        (const dftu_svc_file_formats*)h->get_service(h->h,
                                                     DFTU_SVC_FILE_FORMATS);
    if (!formats || !formats->register_sink)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no sink registry");

    /* The status is returned as it is, so the host's own message on `ctx`
     * (the sink name and the missing callback) is what fails the load. */
    const dftu_status st = formats->register_sink(ctx, h->h, "toy_bad_sink.out",
                                                  &BAD_SINK_VT, NULL);
    if (st < 0) return st;

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
