// SPDX-License-Identifier: MIT
/* Conformance plugin: touches every dftu.svc.* service slot from real C,
 * plus the provider-schema-types and plan-node surfaces, and reports one
 * pass/fail line per service through dftu_svc_result. A single artifact
 * proving the whole ABI is reachable and usable, not merely that its headers
 * compile as C (c_abi_guard already covers that).
 *
 * Config: "workdir" (required) - a scratch directory for the io/writer/
 * trace/arrow files this plugin writes. "force_missing" (optional) - a
 * dftu.svc.* id to report as unavailable, so a test can prove the failure
 * path is loud rather than silently green.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <fcntl.h>
#include <nanoarrow/nanoarrow.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TRACE_N 5

/* ---- The service table: one entry per dftu.svc.* id. Adding a service adds
   an enumerator here; the switches below have no case for it until someone
   adds one, and they compile with -Werror=switch, so the build fails until
   the new service is wired into this plugin. */
typedef enum {
    REF_SVC_AGG = 0,
    REF_SVC_ARROW,
    REF_SVC_IO,
    REF_SVC_NODES,
    REF_SVC_OPS,
    REF_SVC_PORTS,
    REF_SVC_PROVIDERS,
    REF_SVC_DUQL,
    REF_SVC_RESULT,
    REF_SVC_SKETCH,
    REF_SVC_TASK,
    REF_SVC_TRACE,
    REF_SVC_FILEIO,
    REF_SVC_COUNT
} ref_svc_id;

typedef enum {
    REF_PENDING = 0,
    REF_OK = 1,
    REF_FAIL = -1,
    REF_MISSING = -2
} ref_status;

static const char* ref_svc_name(ref_svc_id id) {
    switch (id) {
        case REF_SVC_AGG:
            return "agg";
        case REF_SVC_ARROW:
            return "arrow";
        case REF_SVC_IO:
            return "io";
        case REF_SVC_NODES:
            return "nodes";
        case REF_SVC_OPS:
            return "ops";
        case REF_SVC_PORTS:
            return "ports";
        case REF_SVC_PROVIDERS:
            return "providers";
        case REF_SVC_DUQL:
            return "duql";
        case REF_SVC_RESULT:
            return "result";
        case REF_SVC_SKETCH:
            return "sketch";
        case REF_SVC_TASK:
            return "task";
        case REF_SVC_TRACE:
            return "trace";
        case REF_SVC_FILEIO:
            return "fileio";
        case REF_SVC_COUNT:
            break;
    }
    return "(unknown)";
}

static const char* ref_svc_ext_id(ref_svc_id id) {
    switch (id) {
        case REF_SVC_AGG:
            return DFTU_SVC_AGG;
        case REF_SVC_ARROW:
            return DFTU_SVC_ARROW;
        case REF_SVC_IO:
            return DFTU_SVC_IO;
        case REF_SVC_NODES:
            return DFTU_SVC_NODES;
        case REF_SVC_OPS:
            return DFTU_SVC_OPS;
        case REF_SVC_PORTS:
            return DFTU_SVC_PORTS;
        case REF_SVC_PROVIDERS:
            return DFTU_SVC_PROVIDERS;
        case REF_SVC_DUQL:
            return DFTU_SVC_DUQL;
        case REF_SVC_RESULT:
            return DFTU_SVC_RESULT;
        case REF_SVC_SKETCH:
            return DFTU_SVC_SKETCH;
        case REF_SVC_TASK:
            return DFTU_SVC_TASK;
        case REF_SVC_TRACE:
            return DFTU_SVC_TRACE;
        case REF_SVC_FILEIO:
            return DFTU_SVC_FILEIO;
        case REF_SVC_COUNT:
            break;
    }
    return "";
}

static int32_t g_status[REF_SVC_COUNT];
static int32_t g_provider_ok = REF_PENDING;
static int32_t g_node_ok = REF_PENDING;
static int32_t g_state_ok = REF_PENDING;
static int32_t g_decimal_field_index = -1;
static char g_force_missing[128];

/* Simulated get_service: reports REF_MISSING for the one id under test
   without ever calling the real host, so a test can prove the report is
   loud instead of quietly passing. */
static const void* ref_get_service(const dftu_plugin_host* host,
                                   ref_svc_id id) {
    const char* ext_id = ref_svc_ext_id(id);
    if (g_force_missing[0] != '\0' && strcmp(g_force_missing, ext_id) == 0) {
        g_status[id] = REF_MISSING;
        return NULL;
    }
    {
        const void* svc = host->get_service(host->h, ext_id);
        if (!svc) {
            g_status[id] = REF_MISSING;
            host->log(host->h, DFTU_LOG_ERROR, ext_id,
                      (uint32_t)strlen(ext_id));
        }
        return svc;
    }
}

/* ---- Provider: "reference_plugin.src", columns n (Int64) and amount
   (Decimal128(10,2), a parameterized type). */

static const char* SRC_NAMES[2] = {"n", "amount"};

static dftu_status src_schema(dftu_ctx* ctx, void* self,
                              const char* const** out_names,
                              int32_t* out_count) {
    (void)ctx;
    (void)self;
    *out_names = SRC_NAMES;
    *out_count = 2;
    return DFTU_OK;
}

static dftu_status src_schema_types(dftu_ctx* ctx, void* self,
                                    dftu_schema* out) {
    (void)ctx;
    (void)self;
    dftu_schema_add_field(out, "n", DFTU_TYPE_INT64, 0, DFTU_TIME_UNIT_SECOND,
                          NULL, 0, 0, 0);
    g_decimal_field_index =
        dftu_schema_add_field(out, "amount", DFTU_TYPE_DECIMAL128, 0,
                              DFTU_TIME_UNIT_SECOND, NULL, 10, 2, 0);
    return DFTU_OK;
}

typedef struct {
    int done;
} SrcCursor;

static dftu_status src_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                            dftu_dataframe** out, dftu_task** pending) {
    SrcCursor* cur = (SrcCursor*)self;
    (void)ctx;
    (void)max_rows;
    (void)pending;
    *out = NULL;
    if (cur->done) return DFTU_END;
    cur->done = 1;
    {
        static const int64_t n_vals[3] = {1, 2, 3};
        /* Decimal128 values, 16 bytes each, little-endian: 1.00 2.00 3.00 */
        static const int64_t amount_vals[6] = {100, 0, 200, 0, 300, 0};
        dftu_series* n_col =
            dftu_series_new_flat(DFTU_TYPE_INT64, n_vals, 3, NULL);
        dftu_series* amount_col =
            dftu_series_new_flat(DFTU_TYPE_DECIMAL128, amount_vals, 3, NULL);
        dftu_series* cols[2];
        cols[0] = n_col;
        cols[1] = amount_col;
        *out = dftu_dataframe_new(SRC_NAMES, cols, 2);
    }
    return DFTU_OK;
}

static void src_cursor_destroy(void* self) { free(self); }

static const dftu_cursor_vt SRC_CURSOR_VT = {sizeof(dftu_cursor_vt), src_next,
                                             src_cursor_destroy, NULL};

static dftu_status src_scan(dftu_ctx* ctx, void* self,
                            const dftu_plugin_host* host,
                            const dftu_scan_request* req, int32_t* out_pushed,
                            void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) {
    SrcCursor* cur;
    (void)self;
    (void)host;
    (void)req;
    (void)out_pushed;
    cur = (SrcCursor*)calloc(1, sizeof(SrcCursor));
    if (!cur) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    *out_cursor_self = cur;
    *out_vt = &SRC_CURSOR_VT;
    return DFTU_OK;
}

static void src_destroy(void* self) { (void)self; }

static const dftu_source_vt SRC_VT = {
    sizeof(dftu_source_vt), src_schema, src_scan, src_destroy,
    src_schema_types,       NULL};

/* ---- Node: "reference_plugin.node", doubles column "n", passes "amount"
 * through unchanged.
 *
 * A node's next() gets no dftu_plugin_host*, and the host open() gets serves no
 * dftu_svc_task, so a node cannot reach dftu_svc_coro to await its upstream,
 * and plain C has no native coroutine to co_await it with either.
 * dftu_svc_task::run is the way around that: it blocks the calling thread
 * until a task ends, so next() can synchronously resolve its upstream before
 * transforming the frame. The factory keeps the task service and its `h` (run
 * uses no host state, so they outlive the factory's host) for the node to
 * call. open() uses its host for the memory service: the cursor reserves its
 * working bytes in the pool of the plugin and releases them when destroyed.
 */

static const dftu_svc_task* g_node_task = NULL;
static void* g_node_task_h = NULL;

typedef struct {
    void* in_self;
    const dftu_cursor_vt* in_vt;
    const dftu_svc_memory* mem;
    dftu_reservation* res;
} NodeCursor;

static dftu_status node_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                             dftu_dataframe** out, dftu_task** pending) {
    NodeCursor* nc = (NodeCursor*)self;
    dftu_task* t = NULL;
    dftu_dataframe* frame = NULL;
    dftu_status st = nc->in_vt->next(ctx, nc->in_self, max_rows, &frame, &t);
    (void)pending;
    *out = NULL;
    if (st >= 0 && t) {
        st = g_node_task->run(ctx, g_node_task_h, t);
        if (st < 0) {
            if (frame) dftu_dataframe_free(frame);
            return st;
        }
    }
    if (st < 0) return st;
    if (st == DFTU_END || !frame) return DFTU_END;
    {
        dftu_series* n_col = dftu_dataframe_column(frame, "n");
        dftu_series* amount_col = dftu_dataframe_column(frame, "amount");
        const int64_t rows = dftu_series_length(n_col);
        const int64_t* n_data = (const int64_t*)dftu_series_data(n_col);
        int64_t* doubled = (int64_t*)malloc(sizeof(int64_t) * (size_t)rows);
        int64_t i;
        for (i = 0; i < rows; ++i) doubled[i] = n_data[i] * 2;
        {
            dftu_series* doubled_col =
                dftu_series_new_flat(DFTU_TYPE_INT64, doubled, rows, NULL);
            dftu_series* cols[2];
            cols[0] = doubled_col;
            cols[1] = amount_col;
            *out = dftu_dataframe_new(SRC_NAMES, cols, 2);
        }
        free(doubled);
        dftu_series_free(n_col);
        dftu_dataframe_free(frame);
    }
    return DFTU_OK;
}

static void node_cursor_destroy(void* self) {
    NodeCursor* nc = (NodeCursor*)self;
    nc->mem->release(nc->res);
    if (nc->in_vt->destroy) nc->in_vt->destroy(nc->in_self);
    free(nc);
}

/* The node keeps every input column at its position (it rewrites `n` in
 * place), so a narrowing over its output is one over its input as well and
 * goes straight up; a node that dropped or reordered columns would remap or
 * refuse. */
static dftu_status node_narrow(dftu_ctx* ctx, void* self,
                               const dftu_expr* predicate, int32_t* out_applied,
                               dftu_task** pending) {
    NodeCursor* nc = (NodeCursor*)self;
    if (!nc->in_vt->narrow) {
        *out_applied = 0;
        return DFTU_OK;
    }
    return nc->in_vt->narrow(ctx, nc->in_self, predicate, out_applied, pending);
}

static const dftu_cursor_vt NODE_CURSOR_VT = {sizeof(dftu_cursor_vt), node_next,
                                              node_cursor_destroy, node_narrow};

static dftu_status node_output_schema(dftu_ctx* ctx, void* self,
                                      const dftu_schema* in,
                                      const dftu_op_arg* args,
                                      dftu_schema* out) {
    int32_t n, i;
    (void)ctx;
    (void)self;
    (void)args;
    n = dftu_schema_field_count(in);
    for (i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
    return DFTU_OK;
}

static dftu_status node_open(dftu_ctx* ctx, void* self,
                             const dftu_plugin_host* host, void* in_cursor_self,
                             const dftu_cursor_vt* in_vt,
                             const dftu_op_arg* args, uint64_t memory_budget,
                             void** out_cursor_self,
                             const dftu_cursor_vt** out_vt) {
    NodeCursor* nc;
    (void)self;
    (void)args;
    (void)memory_budget;
    nc = (NodeCursor*)malloc(sizeof(NodeCursor));
    if (!nc) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    nc->in_self = in_cursor_self;
    nc->in_vt = in_vt;
    nc->res = NULL;
    nc->mem =
        (const dftu_svc_memory*)host->get_service(host->h, DFTU_SVC_MEMORY);
    if (!nc->mem) {
        free(nc);
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED,
                         "the cursor host has no memory service");
    }
    {
        dftu_status st =
            nc->mem->reserve(ctx, host->h, &nc->res, "reference node");
        if (st >= 0) st = nc->mem->try_grow(ctx, nc->res, 4096);
        if (st < 0) {
            nc->mem->release(nc->res);
            free(nc);
            return st;
        }
    }
    *out_cursor_self = nc;
    *out_vt = &NODE_CURSOR_VT;
    return DFTU_OK;
}

static void node_destroy(void* self) { (void)self; }

static const dftu_node_vt NODE_VT = {sizeof(dftu_node_vt), node_output_schema,
                                     node_open, node_destroy};

/* ---- A registered op: "reference_plugin.count_rows" (series -> i64). */

static int64_t count_rows(const dftu_series* v) {
    return dftu_series_length(v);
}

static const dftu_op_desc COUNT_ROWS_OP = {"reference_plugin.count_rows",
                                           DFTU_OP_SIG(I64, SERIES, NONE, NONE),
                                           (dftu_op_generic_fn)&count_rows};

/* ---- Task leaf job: doubles an int64 value, inline. */

static dftu_status double_fn(dftu_ctx* ctx, void* state, const void* in,
                             void* out, dftu_task** pending) {
    (void)ctx;
    (void)state;
    (void)pending;
    *(int64_t*)out = *(const int64_t*)in * 2;
    return DFTU_OK;
}

/* ---- A registered mergeable state (dftu_svc_agg::register_state, tier 2 of
   the agg service): sums the "dur" column across every batch. Registered from
   the factory, driven entirely by the host - update per batch, merge at the
   worker/master fan-in, serialize/deserialize past the memory budget,
   finalize once - so on_batch and on_finalize never touch it directly. */

#define DUR_SUM_STATE_NAME "reference_plugin.dur_sum"

typedef struct {
    uint64_t sum;
} DurSumState;

static dftu_status dur_sum_init(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(DurSumState));
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    return DFTU_OK;
}

static dftu_status dur_sum_update(dftu_ctx* ctx, void* state,
                                  const dftu_dataframe* df) {
    DurSumState* s = (DurSumState*)state;
    dftu_series* dur = dftu_dataframe_column(df, "dur");
    if (!dur)
        return dftu_fail(ctx, DFTU_E_NOT_FOUND,
                         "reference_plugin.dur_sum: no dur column");
    {
        const int64_t rows = dftu_series_length(dur);
        const uint64_t* vals = (const uint64_t*)dftu_series_data(dur);
        int64_t i;
        for (i = 0; i < rows; ++i) s->sum += vals[i];
    }
    dftu_series_free(dur);
    return DFTU_OK;
}

static dftu_status dur_sum_merge(dftu_ctx* ctx, void* into, void* other) {
    (void)ctx;
    ((DurSumState*)into)->sum += ((DurSumState*)other)->sum;
    return DFTU_OK;
}

static uint64_t dur_sum_bytes(const void* state) {
    (void)state;
    return sizeof(DurSumState);
}

static void dur_sum_free_buf(void* data, void* ud) {
    (void)ud;
    free(data);
}

static dftu_status dur_sum_serialize(dftu_ctx* ctx, const void* state,
                                     dftu_bytes* out) {
    uint64_t* buf = (uint64_t*)malloc(sizeof(uint64_t));
    if (!buf) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    *buf = ((const DurSumState*)state)->sum;
    out->data = buf;
    out->len = sizeof(uint64_t);
    out->free_fn = dur_sum_free_buf;
    out->ud = NULL;
    return DFTU_OK;
}

static dftu_status dur_sum_deserialize(dftu_ctx* ctx, void* self, dftu_bytes in,
                                       void** out) {
    DurSumState* s;
    (void)self;
    s = (DurSumState*)calloc(1, sizeof(DurSumState));
    if (!s) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    if (in.data && in.len == sizeof(uint64_t))
        memcpy(&s->sum, in.data, sizeof(uint64_t));
    *out = s;
    return DFTU_OK;
}

static uint64_t g_dur_sum_result;

static dftu_status dur_sum_finalize(dftu_ctx* ctx, void* state,
                                    dftu_result_value* out) {
    (void)ctx;
    g_dur_sum_result = ((DurSumState*)state)->sum;
    out->kind = DFTU_RESULT_KIND_BYTES;
    out->u.bytes.data = &g_dur_sum_result;
    out->u.bytes.len = sizeof(g_dur_sum_result);
    return DFTU_OK;
}

static void dur_sum_destroy(void* state) { free(state); }

/* ---- task spawn probe: runs as a blocking job. */

static dftu_status bump_fn(dftu_ctx* ctx, void* arg) {
    (void)ctx;
    *(int64_t*)arg += 1;
    return DFTU_OK;
}

/* ---- A small trace frame (cat/name/ph/pid/tid/ts/dur) for the trace
   round-trip check. */

static dftu_series* make_string_col(const char* const* vals, int64_t n) {
    int32_t* offsets = (int32_t*)malloc(sizeof(int32_t) * (size_t)(n + 1));
    size_t total = 0;
    int64_t i;
    char* data;
    dftu_series* col;
    offsets[0] = 0;
    for (i = 0; i < n; ++i) {
        total += strlen(vals[i]);
        offsets[i + 1] = (int32_t)total;
    }
    data = (char*)malloc(total ? total : 1);
    {
        size_t pos = 0;
        for (i = 0; i < n; ++i) {
            size_t l = strlen(vals[i]);
            memcpy(data + pos, vals[i], l);
            pos += l;
        }
    }
    col = dftu_series_new_string(DFTU_TYPE_STRING, offsets, data, n, NULL);
    free(offsets);
    free(data);
    return col;
}

static dftu_dataframe* build_trace_frame(void) {
    const char* col_names[7] = {"cat", "name", "ph", "pid", "tid", "ts", "dur"};
    const char* cat_vals[TRACE_N];
    const char* name_vals[TRACE_N];
    int64_t ph[TRACE_N];
    uint64_t pid[TRACE_N], tid[TRACE_N], ts[TRACE_N], dur[TRACE_N];
    dftu_series* cols[7];
    int i;
    for (i = 0; i < TRACE_N; ++i) {
        cat_vals[i] = "POSIX";
        name_vals[i] = "write";
        ph[i] = DFTU_PH_COMPLETE;
        pid[i] = 7;
        tid[i] = 9;
        ts[i] = (uint64_t)(1000000 + i * 100);
        dur[i] = 3;
    }
    cols[0] = make_string_col(cat_vals, TRACE_N);
    cols[1] = make_string_col(name_vals, TRACE_N);
    cols[2] = dftu_series_new_flat(DFTU_TYPE_INT64, ph, TRACE_N, NULL);
    cols[3] = dftu_series_new_flat(DFTU_TYPE_UINT64, pid, TRACE_N, NULL);
    cols[4] = dftu_series_new_flat(DFTU_TYPE_UINT64, tid, TRACE_N, NULL);
    cols[5] = dftu_series_new_flat(DFTU_TYPE_UINT64, ts, TRACE_N, NULL);
    cols[6] = dftu_series_new_flat(DFTU_TYPE_UINT64, dur, TRACE_N, NULL);
    return dftu_dataframe_new(col_names, cols, 7);
}

static dftu_status trace_read_cb(dftu_ctx* ctx, const void* item, void* ud) {
    const dftu_dataframe* df = (const dftu_dataframe*)item;
    (void)ctx;
    *(int64_t*)ud += dftu_dataframe_num_rows(df);
    return DFTU_OK;
}

/* ---- Per-worker slice and the batch-time services (agg, query, ops, ports,
   sketch: all synchronous, so they run directly in on_batch). */

typedef struct {
    char workdir[512];
} RefConfig;

typedef struct {
    const RefConfig* cfg;
    dftu_sketch* sketch;
    dftu_agg* agg;
    int64_t rows_seen;
    int started;
} RefSlice;

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    RefSlice* s = (RefSlice*)calloc(1, sizeof(RefSlice));
    if (!s) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    s->cfg = (const RefConfig*)self;
    *out = s;
    return DFTU_OK;
}

static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
    RefSlice* a = (RefSlice*)into;
    RefSlice* b = (RefSlice*)other;
    (void)ctx;
    a->rows_seen += b->rows_seen;
    /* merge() gets no host, so a raw handle like dftu_sketch* cannot be
       folded with sketch_merge here; move it instead. The master fold's own
       slice (on_finalize's argument) never runs on_batch itself, so this is
       the only path a worker's sketch reaches finalize. Correct for a scan
       that never slices into more than one worker unit, as this plugin's
       tests always do; a second non-empty slice would leak its sample into
       nothing rather than being merged. */
    if (!a->sketch) {
        a->sketch = b->sketch;
        b->sketch = NULL;
    }
    if (!a->agg) a->agg = b->agg;
    return DFTU_OK;
}

static void destroy_slice(void* slice) { free(slice); }

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    RefSlice* s = (RefSlice*)slice;
    const int64_t rows = dftu_dataframe_num_rows(df);
    s->rows_seen += rows;

    if (!s->started) {
        s->started = 1;

        {
            const dftu_svc_sketch* sk =
                (const dftu_svc_sketch*)ref_get_service(host, REF_SVC_SKETCH);
            if (sk) s->sketch = sk->sketch_create(host->h);
        }

        {
            const dftu_svc_agg* agg =
                (const dftu_svc_agg*)ref_get_service(host, REF_SVC_AGG);
            if (agg) {
                const char* key_names[1] = {"cat"};
                const dftu_agg_col specs[1] = {
                    {DFTU_AGG_COUNT, NULL, "n", 0.0, NULL}};
                g_status[REF_SVC_AGG] =
                    agg->agg_new(ctx, host->h, "reference_plugin.agg",
                                 key_names, 1, specs, 1, &s->agg) >= 0
                        ? REF_OK
                        : REF_FAIL;
            }
        }

        {
            const dftu_svc_duql* q =
                (const dftu_svc_duql*)ref_get_service(host, REF_SVC_DUQL);
            if (q) {
                const char src[] = "cat == \"POSIX\"";
                dftu_duql* compiled = NULL;
                int matched = -1;
                if (q->duql_compile(ctx, host->h, src,
                                    (uint32_t)(sizeof(src) - 1),
                                    &compiled) >= 0)
                    matched = q->duql_matches(host->h, compiled, df, 0);
                g_status[REF_SVC_DUQL] = matched == 1 ? REF_OK : REF_FAIL;
            }
        }

        {
            const dftu_svc_ops* ops =
                (const dftu_svc_ops*)ref_get_service(host, REF_SVC_OPS);
            if (ops) {
                dftu_series* cat = dftu_dataframe_column(df, "cat");
                int ops_ok;
                dftu_series* hashed = NULL;
                ops_ok = ops->run(ctx, host->h, "dftu.hash.fnv1a",
                                  (const dftu_series* const*)&cat, 1, NULL,
                                  &hashed) >= 0;
                if (ops_ok) dftu_series_free(hashed);

                ops_ok =
                    ops_ok &&
                    (ops->find(host->h, "reference_plugin.count_rows") != NULL);

                {
                    dftu_scalar counted;
                    ops_ok = ops_ok &&
                             ops->run_aggregate(ctx, host->h,
                                                "reference_plugin.count_rows",
                                                (const dftu_series* const*)&cat,
                                                1, NULL, &counted) >= 0 &&
                             counted.value.i == rows;
                }

                dftu_series_free(cat);
                g_status[REF_SVC_OPS] = ops_ok ? REF_OK : REF_FAIL;
            }
        }

        {
            const dftu_svc_ports* ports =
                (const dftu_svc_ports*)ref_get_service(host, REF_SVC_PORTS);
            if (ports) {
                static const char payload[] = "reference-port";
                uint64_t key =
                    ports->port_key(host->h, "reference_plugin.marker");
                uint32_t out_len = 0;
                const void* got;
                ports->publish(host->h, key, payload,
                               (uint32_t)sizeof(payload));
                got = ports->consume(host->h, key, &out_len);
                g_status[REF_SVC_PORTS] =
                    (got && out_len == sizeof(payload) &&
                     memcmp(got, payload, sizeof(payload)) == 0)
                        ? REF_OK
                        : REF_FAIL;
            }
        }

        {
            /* The run-time host must refuse a provider registration; the
               plugin already registered its provider at load. */
            const dftu_svc_providers* pr =
                (const dftu_svc_providers*)ref_get_service(host,
                                                           REF_SVC_PROVIDERS);
            if (pr) {
                dftu_status rc = pr->register_provider(
                    ctx, host->h, "reference_plugin.rejected", &SRC_VT, NULL);
                g_status[REF_SVC_PROVIDERS] = (rc < 0) ? REF_OK : REF_FAIL;
            }
        }

        {
            /* Likewise for a node: the plugin registered its node at load. */
            const dftu_svc_nodes* nd =
                (const dftu_svc_nodes*)ref_get_service(host, REF_SVC_NODES);
            if (nd) {
                dftu_status rc = nd->register_node(
                    ctx, host->h, "reference_plugin.rejected", &NODE_VT, NULL);
                g_status[REF_SVC_NODES] = (rc < 0) ? REF_OK : REF_FAIL;
            }
        }
    }

    if (s->sketch) {
        const dftu_svc_sketch* sk =
            (const dftu_svc_sketch*)ref_get_service(host, REF_SVC_SKETCH);
        dftu_series* dur = dftu_dataframe_column(df, "dur");
        const uint64_t* vals = (const uint64_t*)dftu_series_data(dur);
        int64_t i;
        for (i = 0; i < rows; ++i)
            sk->sketch_add(host->h, s->sketch, (double)vals[i], 1.0);
        dftu_series_free(dur);
    }
    if (s->agg) {
        const dftu_svc_agg* agg =
            (const dftu_svc_agg*)ref_get_service(host, REF_SVC_AGG);
        const dftu_status st = agg->agg_accumulate(ctx, host->h, s->agg, df);
        if (st < 0) return st;
    }

    return DFTU_OK;
}

/* ---- on_finalize: the async services (io, writer, task) need something to
   sequence them, which C has no native coroutine for, so dftu_svc_task::drive
   plays that role - it calls `step` after every round of tasks it awaits, and
   `step` treats `state` as a program counter. A step reads the status of the
   task it just waited for, then starts the next one. */

typedef struct {
    int state;
    const dftu_plugin_host* host;
    RefSlice* slice;

    dftu_task* last;
    ref_svc_id stage;

    int fd;
    int64_t io_wn, io_rn;
    char io_buf[32];

    dftu_writer* writer;

    int64_t spawned;
    dftu_job* job;
    int64_t job_in, job_out;
} FinalizeCtx;

static void finalize_tail(dftu_ctx* ctx, FinalizeCtx* fc) {
    const dftu_plugin_host* host = fc->host;
    const char* workdir = fc->slice->cfg->workdir;

    /* trace: write TRACE_N events, close, then read them back. */
    {
        const dftu_svc_trace* tr =
            (const dftu_svc_trace*)ref_get_service(host, REF_SVC_TRACE);
        if (tr) {
            char path[600];
            dftu_trace_writer* tw;
            int trace_ok = 0;
            snprintf(path, sizeof(path), "%s/reference_trace.pfw.gz", workdir);
            tw = NULL;
            if (tr->trace_open_write(ctx, host->h, path, &tw) >= 0 && tw) {
                dftu_dataframe* frame = build_trace_frame();
                dftu_status wrc = tr->trace_write(ctx, host->h, tw, frame);
                dftu_status crc;
                dftu_dataframe_free(frame);
                crc = tr->trace_close(ctx, host->h, tw);
                if (wrc >= 0 && crc >= 0) {
                    int64_t read_rows = 0;
                    dftu_status rrc = tr->trace_read(ctx, host->h, path,
                                                     trace_read_cb, &read_rows);
                    trace_ok = (rrc >= 0) && (read_rows == TRACE_N);
                }
            }
            g_status[REF_SVC_TRACE] = trace_ok ? REF_OK : REF_FAIL;
        }
    }

    /* arrow: write a one-column Int64 IPC batch and read it back. */
    {
        const dftu_svc_arrow* ar =
            (const dftu_svc_arrow*)ref_get_service(host, REF_SVC_ARROW);
        if (ar) {
            struct ArrowSchema schema;
            struct ArrowArray array;
            struct ArrowError err;
            int arrow_ok = 0;
            memset(&schema, 0, sizeof(schema));
            memset(&array, 0, sizeof(array));
            /* An IPC batch's top-level schema/array is always a struct of
               columns, even for one column. */
            if (ArrowSchemaInitFromType(&schema, NANOARROW_TYPE_STRUCT) == 0 &&
                ArrowSchemaAllocateChildren(&schema, 1) == 0 &&
                ArrowSchemaInitFromType(schema.children[0],
                                        NANOARROW_TYPE_INT64) == 0 &&
                ArrowSchemaSetName(schema.children[0], "v") == 0 &&
                ArrowArrayInitFromType(&array, NANOARROW_TYPE_STRUCT) == 0 &&
                ArrowArrayAllocateChildren(&array, 1) == 0 &&
                ArrowArrayInitFromType(array.children[0],
                                       NANOARROW_TYPE_INT64) == 0 &&
                ArrowArrayStartAppending(array.children[0]) == 0 &&
                ArrowArrayAppendInt(array.children[0], 42) == 0 &&
                ArrowArrayAppendInt(array.children[0], 43) == 0 &&
                ArrowArrayFinishBuildingDefault(array.children[0], &err) == 0) {
                array.length = 2;
                array.null_count = 0;
                char path[600];
                dftu_status wrc;
                snprintf(path, sizeof(path), "%s/reference_arrow.ipc", workdir);
                wrc = ar->arrow_write_ipc(ctx, host->h, &array, &schema, path);
                if (wrc >= 0) {
                    struct ArrowArray in_array;
                    struct ArrowSchema in_schema;
                    dftu_status rrc;
                    memset(&in_array, 0, sizeof(in_array));
                    memset(&in_schema, 0, sizeof(in_schema));
                    rrc = ar->arrow_read_ipc(ctx, host->h, path, &in_array,
                                             &in_schema);
                    if (rrc >= 0) {
                        if (in_array.length == 2 && in_array.n_children == 1) {
                            struct ArrowArrayView view;
                            ArrowArrayViewInitFromSchema(&view, &in_schema,
                                                         NULL);
                            ArrowArrayViewSetArray(&view, &in_array, NULL);
                            arrow_ok = ArrowArrayViewGetIntUnsafe(
                                           view.children[0], 0) == 42 &&
                                       ArrowArrayViewGetIntUnsafe(
                                           view.children[0], 1) == 43;
                            ArrowArrayViewReset(&view);
                        }
                        if (in_array.release) in_array.release(&in_array);
                        if (in_schema.release) in_schema.release(&in_schema);
                    }
                }
            }
            if (array.release) array.release(&array);
            if (schema.release) schema.release(&schema);
            g_status[REF_SVC_ARROW] = arrow_ok ? REF_OK : REF_FAIL;
        }
    }

    /* providers + node: run a LazyFrame over the registered provider through
       the registered node, and confirm schema_types was reachable. The node
       doubles column "n" ({1, 2, 3} -> {2, 4, 6}). */
    {
        dftu_lazyframe* lf =
            dftu_lazyframe_from_provider("reference_plugin.src");
        if (lf) {
            char* names = dftu_lazyframe_schema(lf);
            int schema_ok = (names != NULL) && (g_decimal_field_index >= 0);
            dftu_lazyframe* lf2;
            int node_ran = 0;
            if (names) dftu_duql_string_free(names);

            lf2 = dftu_lazyframe_op(lf, "reference_plugin.node", NULL);
            if (lf2) {
                dftu_dataframe* collected = dftu_lazyframe_collect(lf2, -1);
                if (collected) {
                    dftu_series* n_col = dftu_dataframe_column(collected, "n");
                    const int64_t* n_data =
                        (const int64_t*)dftu_series_data(n_col);
                    int64_t rows = dftu_dataframe_num_rows(collected);
                    node_ran = rows == 3 && n_data[0] == 2 && n_data[1] == 4 &&
                               n_data[2] == 6;
                    dftu_series_free(n_col);
                    dftu_dataframe_free(collected);
                }
                dftu_lazyframe_free(lf2);
            }
            g_provider_ok = schema_ok ? REF_OK : REF_FAIL;
            g_node_ok = node_ran ? REF_OK : REF_FAIL;
            dftu_lazyframe_free(lf);
        } else {
            g_provider_ok = REF_FAIL;
            g_node_ok = REF_FAIL;
        }
    }

    /* agg + sketch: the whole-scan results, now that every batch has folded
       in. */
    {
        const dftu_svc_agg* agg =
            (const dftu_svc_agg*)ref_get_service(host, REF_SVC_AGG);
        if (agg && fc->slice->agg) {
            dftu_dataframe* result = NULL;
            int agg_ok = agg->agg_result(ctx, host->h, "reference_plugin.agg",
                                         &result) >= 0 &&
                         result && dftu_dataframe_num_rows(result) >= 1;
            if (result) dftu_dataframe_free(result);
            if (g_status[REF_SVC_AGG] == REF_OK)
                g_status[REF_SVC_AGG] = agg_ok ? REF_OK : REF_FAIL;
        }
    }
    {
        const dftu_svc_sketch* sk =
            (const dftu_svc_sketch*)ref_get_service(host, REF_SVC_SKETCH);
        if (sk && fc->slice->sketch) {
            dftu_quantiles q = sk->sketch_result(host->h, fc->slice->sketch);
            int sketch_ok =
                q.count > 0 && q.count == (uint64_t)fc->slice->rows_seen;
            sk->sketch_free(host->h, fc->slice->sketch);
            g_status[REF_SVC_SKETCH] = sketch_ok ? REF_OK : REF_FAIL;
        }
    }

    /* result: emit the aggregated report last, so it reflects every other
       service's final status. */
    {
        const dftu_svc_result* res =
            (const dftu_svc_result*)ref_get_service(host, REF_SVC_RESULT);
        char report[2048];
        if (res) g_status[REF_SVC_RESULT] = REF_OK;
        int off = 0;
        int i;
        int32_t all_ok = 1;
        for (i = 0; i < REF_SVC_COUNT; ++i) {
            const char* st =
                g_status[i] == REF_OK
                    ? "OK"
                    : (g_status[i] == REF_MISSING ? "MISSING" : "FAIL");
            if (g_status[i] != REF_OK) all_ok = 0;
            off += snprintf(report + off, sizeof(report) - (size_t)off,
                            "%s: %s\n", ref_svc_name((ref_svc_id)i), st);
        }
        if (g_provider_ok != REF_OK) all_ok = 0;
        if (g_node_ok != REF_OK) all_ok = 0;
        if (g_state_ok != REF_OK) all_ok = 0;
        off += snprintf(report + off, sizeof(report) - (size_t)off,
                        "provider: %s\nnode: %s\nagg_state: %s\n",
                        g_provider_ok == REF_OK ? "OK" : "FAIL",
                        g_node_ok == REF_OK ? "OK" : "FAIL",
                        g_state_ok == REF_OK
                            ? "OK"
                            : (g_state_ok == REF_MISSING ? "MISSING" : "FAIL"));
        if (res) {
            dftu_result_value v;
            memset(&v, 0, sizeof(v));
            v.kind = DFTU_RESULT_KIND_BYTES;
            v.u.bytes.data = &all_ok;
            v.u.bytes.len = sizeof(all_ok);
            res->emit(ctx, host->h, "reference_ok", &v);

            memset(&v, 0, sizeof(v));
            v.kind = DFTU_RESULT_KIND_BYTES;
            v.u.bytes.data = report;
            v.u.bytes.len = (uint64_t)off;
            res->emit(ctx, host->h, "reference_report", &v);
        }
    }
}

static dftu_status wait_for(FinalizeCtx* fc, dftu_task* task, dftu_task** wait,
                            uint32_t* n_wait) {
    fc->last = task;
    wait[0] = task;
    *n_wait = 1;
    return DFTU_OK;
}

/* Runs the stages in order; every start hands its task to the host, and the
   next step reads its status before it goes on. A failed task marks its
   service FAIL and ends the sequence. */
static dftu_status finalize_step(dftu_ctx* ctx, void* self, dftu_task** wait,
                                 uint32_t cap, uint32_t* n_wait) {
    FinalizeCtx* fc = (FinalizeCtx*)self;
    const dftu_plugin_host* host = fc->host;
    const char* workdir = fc->slice->cfg->workdir;
    const dftu_svc_task* tk =
        (const dftu_svc_task*)ref_get_service(host, REF_SVC_TASK);
    dftu_task* t = NULL;
    char path[600];
    (void)cap;

    if (!tk) return DFTU_END;
    if (fc->last) {
        const dftu_status st = tk->status(ctx, host->h, fc->last);
        fc->last = NULL;
        if (st < 0) {
            g_status[fc->stage] = REF_FAIL;
            fc->state = 9;
        }
    }

    switch (fc->state++) {
        case 0: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            if (!io) break;
            fc->stage = REF_SVC_IO;
            snprintf(path, sizeof(path), "%s/reference_io.bin", workdir);
            {
                const dftu_status started =
                    io->open(ctx, host->h, path, O_CREAT | O_RDWR | O_TRUNC,
                             0644, &fc->fd, &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 1: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            static const char data[] = "reference-io";
            if (!io || fc->fd < 0) break;
            {
                const dftu_status started = io->write(
                    ctx, host->h, fc->fd, data, sizeof(data), &fc->io_wn, &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 2: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            if (!io || fc->fd < 0) break;
            {
                const dftu_status started =
                    io->pread(ctx, host->h, fc->fd, fc->io_buf,
                              sizeof(fc->io_buf), 0, &fc->io_rn, &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 3: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            if (!io || fc->fd < 0) break;
            if (io->close(ctx, host->h, fc->fd, &t) < 0) return DFTU_E_IO;
            return wait_for(fc, t, wait, n_wait);
        }
        case 4: {
            static const char data[] = "reference-io";
            const dftu_svc_fileio* w;
            int io_ok = fc->fd >= 0 && fc->io_wn == (int64_t)sizeof(data) &&
                        fc->io_rn == (int64_t)sizeof(data) &&
                        memcmp(fc->io_buf, data, sizeof(data)) == 0;
            g_status[REF_SVC_IO] = io_ok ? REF_OK : REF_FAIL;

            w = (const dftu_svc_fileio*)ref_get_service(host, REF_SVC_FILEIO);
            if (!w) break;
            fc->stage = REF_SVC_FILEIO;
            snprintf(path, sizeof(path), "%s/reference_writer.out", workdir);
            if (w->writer_create(ctx, host->h, path, 1, 0, &fc->writer) < 0 ||
                !fc->writer) {
                g_status[REF_SVC_FILEIO] = REF_FAIL;
                break;
            }
            {
                const dftu_status started =
                    w->writer_open(ctx, host->h, fc->writer, &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 5: {
            const dftu_svc_fileio* w =
                (const dftu_svc_fileio*)ref_get_service(host, REF_SVC_FILEIO);
            static const char chunk[] = "reference-writer-chunk";
            if (!w || !fc->writer) break;
            {
                const dftu_status started = w->writer_chunk(
                    ctx, host->h, fc->writer, 0, chunk, sizeof(chunk), &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 6: {
            const dftu_svc_fileio* w =
                (const dftu_svc_fileio*)ref_get_service(host, REF_SVC_FILEIO);
            if (!w || !fc->writer) break;
            {
                const dftu_status started =
                    w->writer_close(ctx, host->h, fc->writer, &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 7: {
            struct stat st;
            snprintf(path, sizeof(path), "%s/reference_writer.out.shard_0",
                     workdir);
            g_status[REF_SVC_FILEIO] =
                (fc->writer && stat(path, &st) == 0 && st.st_size > 0)
                    ? REF_OK
                    : REF_FAIL;

            /* A blocking spawn: the one task slot that runs a plain C
               function. */
            fc->stage = REF_SVC_TASK;
            {
                const dftu_status started = tk->spawn(
                    ctx, host->h, bump_fn, &fc->spawned, DFTU_JOB_BLOCKING, &t);
                if (started < 0) return started;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        case 8: {
            dftu_job_desc desc;
            int spawn_ok = fc->spawned == 1;
            if (!spawn_ok) g_status[REF_SVC_TASK] = REF_FAIL;
            memset(&desc, 0, sizeof(desc));
            desc.struct_size = sizeof(desc);
            desc.where = DFTU_JOB_INLINE;
            desc.fn = double_fn;
            desc.in_ty = DFTU_T_I64;
            desc.in_size = sizeof(int64_t);
            desc.out_ty = DFTU_T_I64;
            desc.out_size = sizeof(int64_t);
            if (tk->job(ctx, host->h, &desc, &fc->job) < 0) {
                g_status[REF_SVC_TASK] = REF_FAIL;
                break;
            }
            fc->job_in = 21;
            if (tk->start(ctx, host->h, fc->job, &fc->job_in, &fc->job_out,
                          &t) < 0) {
                g_status[REF_SVC_TASK] = REF_FAIL;
                break;
            }
            return wait_for(fc, t, wait, n_wait);
        }
        default: {
            int task_ok =
                fc->spawned == 1 && fc->job && fc->job_out == fc->job_in * 2;
            g_status[REF_SVC_TASK] = task_ok ? REF_OK : REF_FAIL;
            break;
        }
    }

    finalize_tail(ctx, fc);
    return DFTU_END;
}

static void finalize_release(void* coro) { free(coro); }

static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending) {
    RefSlice* s = (RefSlice*)slice;
    const dftu_svc_task* tk;
    FinalizeCtx* fc = (FinalizeCtx*)calloc(1, sizeof(FinalizeCtx));
    if (!fc) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    fc->host = host;
    fc->slice = s;

    tk = (const dftu_svc_task*)ref_get_service(host, REF_SVC_TASK);
    if (!tk) {
        free(fc);
        return DFTU_OK;
    }
    {
        const dftu_status st = tk->drive(ctx, host->h, finalize_step, fc,
                                         finalize_release, pending);
        if (st < 0) free(fc);
        return st;
    }
}

static const char* READS[] = {"cat", "dur", NULL};
static const char* const* reads(void* self) {
    (void)self;
    return READS;
}

static const dftu_config_key CONFIG_KEYS[] = {
    {"workdir", DFTU_VAL_STR, 1,
     "scratch directory for io/writer/trace/arrow files"},
    {"force_missing", DFTU_VAL_STR, 0,
     "a dftu.svc.* id to report as unreachable"},
    {NULL, 0, 0, NULL}};
static const dftu_config_key* config_keys(void* self) {
    (void)self;
    return CONFIG_KEYS;
}

static void destroy(void* self) {
    RefConfig* cfg = (RefConfig*)self;
    dftu_node_unregister("reference_plugin.node");
    g_node_task = NULL;
    g_node_task_h = NULL;
    dftu_provider_unregister("reference_plugin.src");
    dftu_op_unregister("reference_plugin.count_rows");
    memset(g_status, 0, sizeof(g_status));
    g_provider_ok = REF_PENDING;
    g_node_ok = REF_PENDING;
    g_state_ok = REF_PENDING;
    g_dur_sum_result = 0;
    g_decimal_field_index = -1;
    g_force_missing[0] = '\0';
    free(cfg);
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("reference_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    RefConfig* cfg;
    const dftu_value* workdir_v;
    const char* workdir_s;
    uint32_t workdir_len = 0;
    const dftu_svc_ops* ops;
    const dftu_svc_providers* providers;

    workdir_v = dftu_obj_get(config, "workdir");
    workdir_s = dftu_as_str(workdir_v, &workdir_len);
    if (!workdir_s || workdir_len == 0 || workdir_len >= 500)
        return dftu_fail(ctx, DFTU_E_INVALID, "workdir is missing or too long");

    cfg = (RefConfig*)calloc(1, sizeof(RefConfig));
    if (!cfg) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    memcpy(cfg->workdir, workdir_s, workdir_len);
    cfg->workdir[workdir_len] = '\0';

    {
        const dftu_value* fm_v = dftu_obj_get(config, "force_missing");
        uint32_t fm_len = 0;
        const char* fm_s = dftu_as_str(fm_v, &fm_len);
        if (fm_s && fm_len > 0 && fm_len < sizeof(g_force_missing)) {
            memcpy(g_force_missing, fm_s, fm_len);
            g_force_missing[fm_len] = '\0';
        } else {
            g_force_missing[0] = '\0';
        }
    }

    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (ops && ops->register_op) ops->register_op(ctx, h->h, &COUNT_ROWS_OP);

    providers =
        (const dftu_svc_providers*)h->get_service(h->h, DFTU_SVC_PROVIDERS);
    if (providers && providers->register_provider)
        providers->register_provider(ctx, h->h, "reference_plugin.src", &SRC_VT,
                                     NULL);

    /* register_state is callable only from the factory: by run time the
       registry the host plans from is settled. */
    {
        const dftu_svc_agg* agg_reg =
            (const dftu_svc_agg*)ref_get_service(h, REF_SVC_AGG);
        if (agg_reg && agg_reg->register_state) {
            dftu_state_desc desc;
            memset(&desc, 0, sizeof(desc));
            desc.struct_size = sizeof(desc);
            desc.name = DUR_SUM_STATE_NAME;
            desc.init = dur_sum_init;
            desc.update = dur_sum_update;
            desc.merge = dur_sum_merge;
            desc.bytes = dur_sum_bytes;
            desc.serialize = dur_sum_serialize;
            desc.deserialize = dur_sum_deserialize;
            desc.finalize = dur_sum_finalize;
            desc.destroy = dur_sum_destroy;
            g_state_ok = agg_reg->register_state(ctx, h->h, &desc, NULL) >= 0
                             ? REF_OK
                             : REF_FAIL;
        } else if (!agg_reg) {
            g_state_ok = REF_MISSING;
        } else {
            g_state_ok = REF_FAIL;
        }
    }

    g_node_task = (const dftu_svc_task*)h->get_service(h->h, DFTU_SVC_TASK);
    g_node_task_h = h->h;
    {
        const dftu_svc_nodes* nodes =
            (const dftu_svc_nodes*)h->get_service(h->h, DFTU_SVC_NODES);
        if (nodes && nodes->register_node)
            nodes->register_node(ctx, h->h, "reference_plugin.node", &NODE_VT,
                                 NULL);
    }

    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.self = cfg;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.config_keys = config_keys;
    g_plugin.reads = reads;
    *out = &g_plugin;
    return DFTU_OK;
}
