// SPDX-License-Identifier: MIT
/* Example dftracer-utils plugin: the plugin owns its data structure, the engine
 * only moves the bytes. Each slice builds a pid -> event-count map, merges it
 * across workers, then serializes it to a TSV blob and emits it as a named
 * result. PluginHost::run returns it to Python as {"process_counts": bytes}.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o process_counts.so process_counts.c
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t pid;
    uint64_t count;
} Entry;

typedef struct {
    Entry* entries;
    size_t size;
    size_t cap;
} Counts;

static void counts_add(Counts* c, uint64_t pid, uint64_t n) {
    for (size_t i = 0; i < c->size; ++i) {
        if (c->entries[i].pid == pid) {
            c->entries[i].count += n;
            return;
        }
    }
    if (c->size == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 8;
        Entry* e = (Entry*)realloc(c->entries, cap * sizeof(Entry));
        if (!e) return;
        c->entries = e;
        c->cap = cap;
    }
    c->entries[c->size].pid = pid;
    c->entries[c->size].count = n;
    ++c->size;
}

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(Counts));
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    return DFTU_OK;
}

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    Counts* c = (Counts*)slice;
    (void)ctx;
    int64_t n = dftu_dataframe_num_rows(df);
    dftu_series* pid_col = dftu_dataframe_column(df, "pid");
    const uint64_t* pid = NULL;
    int64_t i;
    (void)host;
    if (pid_col && dftu_series_type(pid_col) == DFTU_TYPE_UINT64)
        pid = (const uint64_t*)dftu_series_data(pid_col);
    if (pid)
        for (i = 0; i < n; ++i) counts_add(c, pid[i], 1);
    if (pid_col) dftu_series_free(pid_col);
    return DFTU_OK;
}

static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
    (void)ctx;
    Counts* a = (Counts*)into;
    const Counts* o = (const Counts*)other;
    for (size_t i = 0; i < o->size; ++i)
        counts_add(a, o->entries[i].pid, o->entries[i].count);
    return DFTU_OK;
}

/* Serialize the merged map to TSV and hand the bytes to the host. Sorted by pid
 * so the output is deterministic regardless of worker interleaving. */
static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending) {
    (void)pending;
    Counts* c = (Counts*)slice;
    const dftu_svc_result* res =
        (const dftu_svc_result*)host->get_service(host->h, DFTU_SVC_RESULT);
    char* buf;
    size_t cap, len;
    dftu_status st;
    if (!res || !res->emit)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "host lacks dftu.svc.result");

    for (size_t i = 0; i + 1 < c->size; ++i)
        for (size_t j = i + 1; j < c->size; ++j)
            if (c->entries[j].pid < c->entries[i].pid) {
                Entry t = c->entries[i];
                c->entries[i] = c->entries[j];
                c->entries[j] = t;
            }

    cap = c->size * 32 + 1;
    buf = (char*)malloc(cap);
    if (!buf) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    len = 0;
    for (size_t i = 0; i < c->size; ++i) {
        int n = snprintf(buf + len, cap - len, "%llu\t%llu\n",
                         (unsigned long long)c->entries[i].pid,
                         (unsigned long long)c->entries[i].count);
        if (n > 0) len += (size_t)n;
    }
    {
        dftu_result_value v;
        v.kind = DFTU_RESULT_KIND_BYTES;
        v.u.bytes.data = buf;
        v.u.bytes.len = (uint64_t)len;
        st = res->emit(ctx, host->h, "process_counts", &v);
    }
    free(buf);
    return st;
}

static void destroy_slice(void* slice) {
    Counts* c = (Counts*)slice;
    if (c) free(c->entries);
    free(c);
}

static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("process_counts", "1.0")

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
    *out = &g_plugin;
    return DFTU_OK;
}
