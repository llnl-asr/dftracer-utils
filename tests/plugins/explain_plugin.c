// SPDX-License-Identifier: MIT
/* Test-only plugin for the explain hooks and the run counters. It registers
 *
 *   explain_plugin.src    a source of ROWS rows (id 1..ROWS, v = 10 * id),
 * MORSEL rows to a frame, with an explain hook. It votes DFTU_PUSHED_INEXACT
 * for the first filter it is offered and DFTU_PUSHED_NO for the others, and
 * counts `scans`, `morsels` and `rows`. explain_plugin.quiet  the same rows
 * with no explain hook and no counter. explain_plugin.pass   a pass-through
 * node with an explain hook that counts `frames`. explain_plugin.fail is a
 * source whose scan always fails. explain_plugin.fail_next is a node that
 * counts `frames`, passes its first frame and fails the next one.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 6
#define MORSEL 3

static const char* NAMES[] = {"id", "v"};

static dftu_status src_schema(dftu_ctx* ctx, void* self,
                              const char* const** out_names,
                              int32_t* out_count) {
    (void)ctx;
    (void)self;
    *out_names = NAMES;
    *out_count = 2;
    return DFTU_OK;
}

typedef struct {
    int64_t next_row;
    int counting;
    int n_columns;
    int columns[2];
} SrcCursor;

static dftu_status src_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                            dftu_dataframe** out, dftu_task** pending) {
    SrcCursor* cur = (SrcCursor*)self;
    int64_t ids[MORSEL];
    int64_t values[MORSEL];
    const char* names[2];
    dftu_series* cols[2];
    int64_t n = 0;
    int c;
    (void)max_rows;
    (void)pending;
    while (cur->next_row < ROWS && n < MORSEL) {
        ids[n] = cur->next_row + 1;
        values[n] = (cur->next_row + 1) * 10;
        ++cur->next_row;
        ++n;
    }
    if (n == 0) {
        *out = NULL;
        return DFTU_END;
    }
    for (c = 0; c < cur->n_columns; ++c) {
        names[c] = NAMES[cur->columns[c]];
        cols[c] = dftu_series_new_flat(
            DFTU_TYPE_INT64, cur->columns[c] == 0 ? ids : values, n, NULL);
    }
    *out = dftu_dataframe_new(names, cols, cur->n_columns);
    if (cur->counting) {
        dftu_counter(ctx, "morsels", 1);
        dftu_counter(ctx, "rows", n);
    }
    return DFTU_OK;
}

static void src_cursor_destroy(void* self) { free(self); }

static const dftu_cursor_vt SRC_CURSOR_VT = {sizeof(dftu_cursor_vt), src_next,
                                             src_cursor_destroy, NULL};

static dftu_status open_cursor(dftu_ctx* ctx, const dftu_scan_request* req,
                               int counting, int32_t* out_pushed,
                               void** out_cursor_self,
                               const dftu_cursor_vt** out_vt) {
    SrcCursor* cur = (SrcCursor*)calloc(1, sizeof(SrcCursor));
    int c;
    if (!cur) return dftu_fail(ctx, DFTU_E_INTERNAL, "no memory");
    cur->counting = counting;
    cur->n_columns = req->n_projection <= 0 ? 2 : req->n_projection;
    if (cur->n_columns > 2) {
        free(cur);
        return dftu_fail(ctx, DFTU_E_INVALID, "explain_plugin: two columns");
    }
    for (c = 0; c < cur->n_columns; ++c) {
        const char* want =
            req->n_projection <= 0 ? NAMES[c] : req->projection[c];
        cur->columns[c] = strcmp(want, "id") == 0 ? 0 : 1;
    }
    if (req->n_filters > 0) out_pushed[0] = DFTU_PUSHED_INEXACT;
    if (counting) dftu_counter(ctx, "scans", 1);
    *out_cursor_self = cur;
    *out_vt = &SRC_CURSOR_VT;
    return DFTU_OK;
}

static dftu_status src_scan(dftu_ctx* ctx, void* self,
                            const dftu_plugin_host* host,
                            const dftu_scan_request* req, int32_t* out_pushed,
                            void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) {
    (void)self;
    (void)host;
    return open_cursor(ctx, req, 1, out_pushed, out_cursor_self, out_vt);
}

static dftu_status quiet_scan(dftu_ctx* ctx, void* self,
                              const dftu_plugin_host* host,
                              const dftu_scan_request* req, int32_t* out_pushed,
                              void** out_cursor_self,
                              const dftu_cursor_vt** out_vt) {
    (void)self;
    (void)host;
    return open_cursor(ctx, req, 0, out_pushed, out_cursor_self, out_vt);
}

static void add(const dftu_explain_out* out, const char* key,
                const char* value) {
    out->add(out->self, key, strlen(key), value, strlen(value));
}

static dftu_status src_explain(dftu_ctx* ctx, void* self,
                               const dftu_scan_request* req,
                               const dftu_explain_out* out) {
    char text[64];
    (void)ctx;
    (void)self;
    snprintf(text, sizeof(text), "%d", (int)req->n_filters);
    add(out, "filters_offered", text);
    add(out, "filters_accepted", req->n_filters > 0 ? "1" : "0");
    snprintf(text, sizeof(text), "%d",
             (int)(req->n_projection <= 0 ? 2 : req->n_projection));
    add(out, "columns", text);
    return DFTU_OK;
}

static void src_destroy(void* self) { (void)self; }

static const dftu_source_vt SRC_VT = {sizeof(dftu_source_vt),
                                      src_schema,
                                      src_scan,
                                      src_destroy,
                                      NULL,
                                      NULL,
                                      src_explain,
                                      NULL,
                                      NULL};

static const dftu_source_vt QUIET_VT = {sizeof(dftu_source_vt),
                                        src_schema,
                                        quiet_scan,
                                        src_destroy,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL};

static dftu_status fail_scan(dftu_ctx* ctx, void* self,
                             const dftu_plugin_host* host,
                             const dftu_scan_request* req, int32_t* out_pushed,
                             void** out_cursor_self,
                             const dftu_cursor_vt** out_vt) {
    (void)self;
    (void)host;
    (void)req;
    (void)out_pushed;
    (void)out_cursor_self;
    (void)out_vt;
    dftu_counter(ctx, "scans", 1);
    return dftu_fail(ctx, DFTU_E_IO, "explain_plugin: the scan fails");
}

static const dftu_source_vt FAIL_VT = {sizeof(dftu_source_vt),
                                       src_schema,
                                       fail_scan,
                                       src_destroy,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL};

typedef struct {
    void* in_self;
    const dftu_cursor_vt* in_vt;
    int frames;
    int fail_after;
} NodeCursor;

static dftu_status node_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                             dftu_dataframe** out, dftu_task** pending) {
    NodeCursor* nc = (NodeCursor*)self;
    dftu_status st;
    if (nc->fail_after > 0 && nc->frames >= nc->fail_after)
        return dftu_fail(ctx, DFTU_E_IO, "explain_plugin: next fails");
    st = nc->in_vt->next(ctx, nc->in_self, max_rows, out, pending);
    if (st >= 0 && *out) {
        dftu_counter(ctx, "frames", 1);
        ++nc->frames;
    }
    return st;
}

static void node_cursor_destroy(void* self) {
    NodeCursor* nc = (NodeCursor*)self;
    if (nc->in_vt->destroy) nc->in_vt->destroy(nc->in_self);
    free(nc);
}

static const dftu_cursor_vt NODE_CURSOR_VT = {sizeof(dftu_cursor_vt), node_next,
                                              node_cursor_destroy, NULL};

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

static dftu_status node_open(dftu_ctx* ctx, void* self,
                             const dftu_plugin_host* host, void* in_self,
                             const dftu_cursor_vt* in_vt,
                             const dftu_op_arg* args, uint64_t budget,
                             void** out_self, const dftu_cursor_vt** out_vt) {
    NodeCursor* nc = (NodeCursor*)calloc(1, sizeof(NodeCursor));
    (void)host;
    (void)args;
    (void)budget;
    if (!nc) return dftu_fail(ctx, DFTU_E_INTERNAL, "no memory");
    nc->in_self = in_self;
    nc->in_vt = in_vt;
    nc->fail_after = self != NULL ? 1 : 0;
    *out_self = nc;
    *out_vt = &NODE_CURSOR_VT;
    return DFTU_OK;
}

static dftu_status node_explain(dftu_ctx* ctx, void* self,
                                const dftu_op_arg* args,
                                const dftu_explain_out* out) {
    (void)ctx;
    (void)self;
    (void)args;
    add(out, "mode", "pass");
    return DFTU_OK;
}

static void node_destroy(void* self) { (void)self; }

static int fail_next_marker;

static const dftu_node_vt NODE_VT = {sizeof(dftu_node_vt), node_schema,
                                     node_open, node_destroy, node_explain};

static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("explain_plugin", "1.0")

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
    st = providers->register_provider(ctx, h->h, "explain_plugin.src", &SRC_VT,
                                      NULL);
    if (st < 0) return st;
    st = providers->register_provider(ctx, h->h, "explain_plugin.quiet",
                                      &QUIET_VT, NULL);
    if (st < 0) return st;
    st = providers->register_provider(ctx, h->h, "explain_plugin.fail",
                                      &FAIL_VT, NULL);
    if (st < 0) return st;
    st = nodes->register_node(ctx, h->h, "explain_plugin.pass", &NODE_VT, NULL);
    if (st < 0) return st;
    st = nodes->register_node(ctx, h->h, "explain_plugin.fail_next", &NODE_VT,
                              &fail_next_marker);
    if (st < 0) return st;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
