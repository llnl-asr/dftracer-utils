// SPDX-License-Identifier: MIT
/* Example consumer plugin: each batch it consumes the producer's per-batch
 * value on the port named com.example.perbatch_dur and accumulates it, logging
 * a total at finalize. It names the port in `consumes`, so the host runs the
 * producer first whatever order the two are given in, and refuses to run at
 * all when no loaded plugin provides that port.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o perbatch_consumer.so perbatch_consumer.c
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERBATCH_PORT "com.example.perbatch_dur"

typedef struct {
    uint64_t total;
    uint64_t batches;
} Slice;

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(Slice));
    if (!*out) return dftu_fail(ctx, DFTU_E_INTERNAL, "out of memory");
    return DFTU_OK;
}

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    (void)ctx;
    (void)df;
    Slice* s = (Slice*)slice;
    const dftu_svc_ports* p =
        (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);
    if (p) {
        uint32_t len = 0;
        const void* d =
            p->consume(host->h, p->port_key(host->h, PERBATCH_PORT), &len);
        if (d && len == (uint32_t)sizeof(uint64_t)) {
            s->total += *(const uint64_t*)d;
            s->batches++;
        }
    }
    return DFTU_OK;
}

static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
    (void)ctx;
    Slice* a = (Slice*)into;
    const Slice* b = (const Slice*)other;
    a->total += b->total;
    a->batches += b->batches;
    return DFTU_OK;
}

static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending) {
    (void)ctx;
    (void)pending;
    const Slice* s = (const Slice*)slice;
    char line[128];
    int n;
    if (s->batches > 0)
        n = snprintf(line, sizeof line,
                     "perbatch_consumer: WIRED total=%llu batches=%llu",
                     (unsigned long long)s->total,
                     (unsigned long long)s->batches);
    else
        n = snprintf(line, sizeof line,
                     "perbatch_consumer: STANDALONE no producer");
    if (n > 0) host->log(host->h, DFTU_LOG_INFO, line, (uint32_t)n);
    return DFTU_OK;
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static const char* const consumed[2] = {PERBATCH_PORT, NULL};

static const char* const* consumes(void* self) {
    (void)self;
    return consumed;
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("perbatch_consumer", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)ctx;
    (void)h;
    (void)config;
    g_plugin.struct_size = sizeof(dftu_plugin);
    g_plugin.self = NULL;
    g_plugin.plan_duql = NULL;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.consumes = consumes;
    *out = &g_plugin;
    return DFTU_OK;
}
