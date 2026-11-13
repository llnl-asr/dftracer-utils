// SPDX-License-Identifier: MIT
/* Test-only plugin that registers a provider and a pass-through node, and
 * appends one byte to the file named by LIFETIME_PLUGIN_MARKER whenever the
 * host calls the provider's, the node's or the plugin's own destroy ('s', 'n'
 * or 'p'). The test
 * unloads the plugin while a plan still holds both and checks that the library
 * stays mapped until the plan is gone. */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mark(char c) {
    const char* path = getenv("LIFETIME_PLUGIN_MARKER");
    FILE* f = path ? fopen(path, "ab") : NULL;
    if (!f) return;
    fputc(c, f);
    fclose(f);
}

static const char* NAMES[] = {"id"};

static dftu_status src_schema(dftu_ctx* ctx, void* self,
                              const char* const** out_names,
                              int32_t* out_count) {
    (void)ctx;
    (void)self;
    *out_names = NAMES;
    *out_count = 1;
    return DFTU_OK;
}

typedef struct {
    int done;
} SrcCursor;

static dftu_status src_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                            dftu_dataframe** out, dftu_task** pending) {
    SrcCursor* cur = (SrcCursor*)self;
    static const int64_t ids[] = {1, 2, 3};
    dftu_series* col;
    (void)ctx;
    (void)max_rows;
    (void)pending;
    if (cur->done) {
        *out = NULL;
        return DFTU_END;
    }
    cur->done = 1;
    col = dftu_series_new_flat(DFTU_TYPE_INT64, ids, 3, NULL);
    *out = dftu_dataframe_new(NAMES, &col, 1);
    return DFTU_OK;
}

static void src_cursor_destroy(void* self) { free(self); }

static const dftu_cursor_vt SRC_CURSOR_VT = {
    sizeof(dftu_cursor_vt), src_next, src_cursor_destroy, NULL, NULL, NULL};

static dftu_status src_scan(dftu_ctx* ctx, void* self,
                            const dftu_scan_request* req, int32_t* out_pushed,
                            void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) {
    (void)self;
    (void)req;
    (void)out_pushed;
    *out_cursor_self = calloc(1, sizeof(SrcCursor));
    if (!*out_cursor_self) return dftu_fail(ctx, DFTU_E_INTERNAL, "no memory");
    *out_vt = &SRC_CURSOR_VT;
    return DFTU_OK;
}

static void src_destroy(void* self) {
    (void)self;
    mark('s');
}

static const dftu_source_vt SRC_VT = {
    sizeof(dftu_source_vt), src_schema, src_scan, src_destroy, NULL, NULL};

typedef struct {
    void* in_self;
    const dftu_cursor_vt* in_vt;
} NodeCursor;

static dftu_status node_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                             dftu_dataframe** out, dftu_task** pending) {
    NodeCursor* nc = (NodeCursor*)self;
    return nc->in_vt->next(ctx, nc->in_self, max_rows, out, pending);
}

static void node_cursor_destroy(void* self) {
    NodeCursor* nc = (NodeCursor*)self;
    if (nc->in_vt->destroy) nc->in_vt->destroy(nc->in_self);
    free(nc);
}

static const dftu_cursor_vt NODE_CURSOR_VT = {
    sizeof(dftu_cursor_vt), node_next, node_cursor_destroy, NULL, NULL, NULL};

static dftu_status node_schema(dftu_ctx* ctx, void* self, const dftu_schema* in,
                               const dftu_op_arg* args, dftu_schema* out) {
    int32_t i;
    const int32_t n = dftu_schema_field_count(in);
    (void)ctx;
    (void)self;
    (void)args;
    for (i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
    return DFTU_OK;
}

static dftu_status node_open(dftu_ctx* ctx, void* self, void* in_self,
                             const dftu_cursor_vt* in_vt,
                             const dftu_op_arg* args, uint64_t budget,
                             void** out_self, const dftu_cursor_vt** out_vt) {
    NodeCursor* nc = (NodeCursor*)malloc(sizeof(NodeCursor));
    (void)self;
    (void)args;
    (void)budget;
    if (!nc) return dftu_fail(ctx, DFTU_E_INTERNAL, "no memory");
    nc->in_self = in_self;
    nc->in_vt = in_vt;
    *out_self = nc;
    *out_vt = &NODE_CURSOR_VT;
    return DFTU_OK;
}

static void node_destroy(void* self) {
    (void)self;
    mark('n');
}

static const dftu_node_vt NODE_VT = {sizeof(dftu_node_vt), node_schema,
                                     node_open, node_destroy};

static void destroy(void* self) {
    (void)self;
    mark('p');
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("lifetime_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_providers* providers;
    const dftu_svc_nodes* nodes;
    dftu_status st;
    (void)config;
    providers =
        (const dftu_svc_providers*)h->get_service(h->h, DFTU_SVC_PROVIDERS);
    nodes = (const dftu_svc_nodes*)h->get_service(h->h, DFTU_SVC_NODES);
    if (!providers || !nodes)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no registry at load");
    st = providers->register_provider(ctx, h->h, "lifetime_plugin.src", &SRC_VT,
                                      NULL);
    if (st < 0) return st;
    st =
        nodes->register_node(ctx, h->h, "lifetime_plugin.pass", &NODE_VT, NULL);
    if (st < 0) return st;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
