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

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, sizeof(Slice));
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
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
    return NULL;
}

static void merge(void* into, void* other) {
    Slice* a = (Slice*)into;
    const Slice* b = (const Slice*)other;
    a->total += b->total;
    a->batches += b->batches;
}

static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
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
    return NULL;
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static const char* const consumed[2] = {PERBATCH_PORT, NULL};

static const char* const* consumes(void* self) {
    (void)self;
    return consumed;
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
    (void)h;
    (void)config;
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.self = NULL;
    g_plugin.plan_query = NULL;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.consumes = consumes;
    return &g_plugin;
}
