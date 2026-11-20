// SPDX-License-Identifier: MIT
/* Test-only plugin for the plan hooks of a source. Every source has the rows
 * id 1..ROWS and v = 10 * id, MORSEL rows to a frame. It registers
 *
 *   plan_plugin.src      a plan hook: a filter `id > N` is voted EXACT with the
 *                        token N, and scan() reads the token and applies it;
 *                        any other filter gets an out-of-range vote, which the
 *                        host reads as NO. scan() writes NO for every filter,
 *                        so only the plan can make the engine drop a filter.
 *                        The plan gives cost 2.5 per row and est_rows ROWS - N,
 *                        and counts `plans`; scan() counts `scans` and adds the
 *                        token it got to `token`.
 *   plan_plugin.noplan   the same rows with no plan hook; scan() votes INEXACT
 *                        for filter 0 and applies nothing.
 *   plan_plugin.dq       a plan hook (est_rows 111) and a plan_duql hook
 *                        (est_rows 222) that keeps the duql text and the filter
 *                        indexes it got; its explain hook prints them.
 *   plan_plugin.dq_only  a plan_duql hook with no plan hook.
 *   plan_plugin.fail     a plan hook that always fails.
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
    int64_t min_id; /* rows with id <= min_id are skipped */
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
    (void)ctx;
    (void)max_rows;
    (void)pending;
    while (n == 0 && cur->next_row < ROWS) {
        int64_t stop = cur->next_row + MORSEL;
        while (cur->next_row < ROWS && cur->next_row < stop) {
            const int64_t id = cur->next_row + 1;
            ++cur->next_row;
            if (id <= cur->min_id) continue;
            ids[n] = id;
            values[n] = id * 10;
            ++n;
        }
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
    return DFTU_OK;
}

static void src_cursor_destroy(void* self) { free(self); }

static const dftu_cursor_vt SRC_CURSOR_VT = {sizeof(dftu_cursor_vt), src_next,
                                             src_cursor_destroy, NULL};

static dftu_status open_cursor(dftu_ctx* ctx, const dftu_scan_request* req,
                               int64_t min_id, void** out_cursor_self,
                               const dftu_cursor_vt** out_vt) {
    SrcCursor* cur = (SrcCursor*)calloc(1, sizeof(SrcCursor));
    int c;
    if (!cur) return dftu_fail(ctx, DFTU_E_INTERNAL, "no memory");
    cur->min_id = min_id;
    cur->n_columns = req->n_projection <= 0 ? 2 : req->n_projection;
    if (cur->n_columns > 2) {
        free(cur);
        return dftu_fail(ctx, DFTU_E_INVALID, "plan_plugin: two columns");
    }
    for (c = 0; c < cur->n_columns; ++c) {
        const char* want =
            req->n_projection <= 0 ? NAMES[c] : req->projection[c];
        cur->columns[c] = strcmp(want, "id") == 0 ? 0 : 1;
    }
    *out_cursor_self = cur;
    *out_vt = &SRC_CURSOR_VT;
    return DFTU_OK;
}

/* The threshold N of the first filter that reads `id > N`, or -1. */
static int first_id_above(const dftu_scan_request* req, int32_t* which,
                          int64_t* threshold) {
    int32_t i;
    for (i = 0; i < req->n_filters; ++i) {
        int32_t col = -1;
        int32_t cmp = -1;
        dftu_scalar rhs;
        if (dftu_expr_as_col_cmp(req->filters[i], &col, &cmp, &rhs) &&
            col == 0 && cmp == DFTU_CMP_GT && rhs.kind == DFTU_SCALAR_TAG_I64) {
            *which = i;
            *threshold = rhs.value.i;
            return 1;
        }
    }
    return 0;
}

static dftu_status src_plan(dftu_ctx* ctx, void* self,
                            const dftu_scan_request* req,
                            dftu_plan_result* out) {
    int32_t which = -1;
    int64_t threshold = 0;
    int32_t i;
    (void)self;
    dftu_counter(ctx, "plans", 1);
    for (i = 0; i < req->n_filters; ++i) out->votes[i] = 99;
    if (first_id_above(req, &which, &threshold)) {
        out->votes[which] = DFTU_PUSHED_EXACT;
        out->token = (uint64_t)threshold;
        out->est_rows = threshold >= ROWS ? 0 : ROWS - threshold;
    } else {
        out->est_rows = ROWS;
    }
    out->cost = 2.5 * (double)out->est_rows;
    return DFTU_OK;
}

static dftu_status src_scan(dftu_ctx* ctx, void* self,
                            const dftu_plugin_host* host,
                            const dftu_scan_request* req, int32_t* out_pushed,
                            void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) {
    int64_t min_id = 0;
    (void)self;
    (void)host;
    dftu_counter(ctx, "scans", 1);
    if (req->plan != NULL) {
        int32_t i;
        for (i = 0; i < req->n_filters; ++i) out_pushed[i] = DFTU_PUSHED_NO;
        min_id = (int64_t)req->plan->token;
        dftu_counter(ctx, "token", min_id);
        if (req->plan->est_rows < 0 || req->plan->cost < 0)
            return dftu_fail(ctx, DFTU_E_INTERNAL,
                             "plan_plugin: the plan was not passed to scan");
    }
    return open_cursor(ctx, req, min_id, out_cursor_self, out_vt);
}

static dftu_status noplan_scan(dftu_ctx* ctx, void* self,
                               const dftu_plugin_host* host,
                               const dftu_scan_request* req,
                               int32_t* out_pushed, void** out_cursor_self,
                               const dftu_cursor_vt** out_vt) {
    (void)self;
    (void)host;
    if (req->n_filters > 0) out_pushed[0] = DFTU_PUSHED_INEXACT;
    if (req->plan != NULL)
        return dftu_fail(ctx, DFTU_E_INTERNAL,
                         "plan_plugin: a plan for a source without a hook");
    return open_cursor(ctx, req, 0, out_cursor_self, out_vt);
}

static void src_destroy(void* self) { (void)self; }

static const dftu_source_vt SRC_VT = {sizeof(dftu_source_vt),
                                      src_schema,
                                      src_scan,
                                      src_destroy,
                                      NULL,
                                      NULL,
                                      NULL,
                                      src_plan,
                                      NULL};

static const dftu_source_vt NOPLAN_VT = {sizeof(dftu_source_vt),
                                         src_schema,
                                         noplan_scan,
                                         src_destroy,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL};

static char g_duql[512];
static char g_index[128];

static dftu_status dq_plan(dftu_ctx* ctx, void* self,
                           const dftu_scan_request* req,
                           dftu_plan_result* out) {
    (void)ctx;
    (void)self;
    (void)req;
    out->est_rows = 111;
    snprintf(g_duql, sizeof(g_duql), "%s", "plan");
    g_index[0] = '\0';
    return DFTU_OK;
}

static dftu_status dq_plan_duql(dftu_ctx* ctx, void* self,
                                const dftu_scan_request* req,
                                const dftu_plan_duql* q,
                                dftu_plan_result* out) {
    int32_t i;
    size_t at = 0;
    (void)ctx;
    (void)self;
    snprintf(g_duql, sizeof(g_duql), "%.*s", (int)q->text_len, q->text);
    g_index[0] = '\0';
    for (i = 0; i < q->n_filters && at + 12 < sizeof(g_index); ++i) {
        at += (size_t)snprintf(g_index + at, sizeof(g_index) - at, "%s%d",
                               i ? "," : "", (int)q->filter_index[i]);
        out->votes[q->filter_index[i]] = DFTU_PUSHED_INEXACT;
    }
    (void)req;
    out->est_rows = 222;
    return DFTU_OK;
}

static dftu_status dq_scan(dftu_ctx* ctx, void* self,
                           const dftu_plugin_host* host,
                           const dftu_scan_request* req, int32_t* out_pushed,
                           void** out_cursor_self,
                           const dftu_cursor_vt** out_vt) {
    (void)self;
    (void)host;
    (void)out_pushed;
    return open_cursor(ctx, req, 0, out_cursor_self, out_vt);
}

static void add(const dftu_explain_out* out, const char* key,
                const char* value) {
    out->add(out->self, key, strlen(key), value, strlen(value));
}

static dftu_status dq_explain(dftu_ctx* ctx, void* self,
                              const dftu_scan_request* req,
                              const dftu_explain_out* out) {
    (void)ctx;
    (void)self;
    add(out, "duql", g_duql);
    add(out, "indexes", g_index);
    {
        char seen[32];
        snprintf(seen, sizeof(seen), "%lld",
                 (long long)(req->plan ? req->plan->est_rows : -2));
        add(out, "seen_est_rows", seen);
    }
    return DFTU_OK;
}

static const dftu_source_vt DQ_VT = {sizeof(dftu_source_vt),
                                     src_schema,
                                     dq_scan,
                                     src_destroy,
                                     NULL,
                                     NULL,
                                     dq_explain,
                                     dq_plan,
                                     dq_plan_duql};

static const dftu_source_vt DQ_ONLY_VT = {sizeof(dftu_source_vt),
                                          src_schema,
                                          dq_scan,
                                          src_destroy,
                                          NULL,
                                          NULL,
                                          dq_explain,
                                          NULL,
                                          dq_plan_duql};

static dftu_status fail_plan(dftu_ctx* ctx, void* self,
                             const dftu_scan_request* req,
                             dftu_plan_result* out) {
    (void)self;
    (void)req;
    (void)out;
    return dftu_fail(ctx, DFTU_E_IO, "plan_plugin: the plan fails");
}

static const dftu_source_vt FAIL_VT = {sizeof(dftu_source_vt),
                                       src_schema,
                                       dq_scan,
                                       src_destroy,
                                       NULL,
                                       NULL,
                                       NULL,
                                       fail_plan,
                                       NULL};

static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("plan_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_providers* providers;
    dftu_status st;
    (void)config;
    providers =
        (const dftu_svc_providers*)h->get_service(h->h, DFTU_SVC_PROVIDERS);
    if (!providers)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no registry at load");
    st = providers->register_provider(ctx, h->h, "plan_plugin.src", &SRC_VT,
                                      NULL);
    if (st < 0) return st;
    st = providers->register_provider(ctx, h->h, "plan_plugin.noplan",
                                      &NOPLAN_VT, NULL);
    if (st < 0) return st;
    st =
        providers->register_provider(ctx, h->h, "plan_plugin.dq", &DQ_VT, NULL);
    if (st < 0) return st;
    st = providers->register_provider(ctx, h->h, "plan_plugin.dq_only",
                                      &DQ_ONLY_VT, NULL);
    if (st < 0) return st;
    st = providers->register_provider(ctx, h->h, "plan_plugin.fail", &FAIL_VT,
                                      NULL);
    if (st < 0) return st;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
