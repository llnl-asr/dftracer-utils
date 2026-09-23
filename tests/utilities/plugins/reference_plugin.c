/* Conformance plugin: touches every dftu.svc.* service slot from real C,
 * plus the provider-schema-types and plan-node surfaces, and reports one
 * pass/fail line per service through dftu_svc_result. Modeled on DuckDB's
 * reference-extension-c: a single artifact proving the whole ABI is
 * reachable and usable, not merely that its headers compile as C
 * (c_abi_guard already covers that).
 *
 * Config: "workdir" (required) - a scratch directory for the io/writer/
 * trace/arrow files this plugin writes. "force_missing" (optional) - a
 * dftu.svc.* id to report as unavailable, so a test can prove the failure
 * path is loud rather than silently green.
 */

#include <dftracer/utils/dataframe/abi.h>
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
    REF_SVC_COMPOSE,
    REF_SVC_CORO,
    REF_SVC_IO,
    REF_SVC_OPS,
    REF_SVC_PORTS,
    REF_SVC_PROVIDERS,
    REF_SVC_QUERY,
    REF_SVC_RESULT,
    REF_SVC_SKETCH,
    REF_SVC_TRACE,
    REF_SVC_WRITER,
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
        case REF_SVC_COMPOSE:
            return "compose";
        case REF_SVC_CORO:
            return "coro";
        case REF_SVC_IO:
            return "io";
        case REF_SVC_OPS:
            return "ops";
        case REF_SVC_PORTS:
            return "ports";
        case REF_SVC_PROVIDERS:
            return "providers";
        case REF_SVC_QUERY:
            return "query";
        case REF_SVC_RESULT:
            return "result";
        case REF_SVC_SKETCH:
            return "sketch";
        case REF_SVC_TRACE:
            return "trace";
        case REF_SVC_WRITER:
            return "writer";
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
        case REF_SVC_COMPOSE:
            return DFTU_SVC_COMPOSE;
        case REF_SVC_CORO:
            return DFTU_SVC_CORO;
        case REF_SVC_IO:
            return DFTU_SVC_IO;
        case REF_SVC_OPS:
            return DFTU_SVC_OPS;
        case REF_SVC_PORTS:
            return DFTU_SVC_PORTS;
        case REF_SVC_PROVIDERS:
            return DFTU_SVC_PROVIDERS;
        case REF_SVC_QUERY:
            return DFTU_SVC_QUERY;
        case REF_SVC_RESULT:
            return DFTU_SVC_RESULT;
        case REF_SVC_SKETCH:
            return DFTU_SVC_SKETCH;
        case REF_SVC_TRACE:
            return DFTU_SVC_TRACE;
        case REF_SVC_WRITER:
            return DFTU_SVC_WRITER;
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

static int32_t src_schema(void* self, const char* const** out_names) {
    (void)self;
    *out_names = SRC_NAMES;
    return 2;
}

static void src_schema_types(void* self, dftu_schema* out) {
    (void)self;
    dftu_schema_add_field(out, "n", DFTU_TYPE_INT64, 0, DFTU_TIME_UNIT_SECOND,
                          NULL, 0, 0, 0);
    g_decimal_field_index =
        dftu_schema_add_field(out, "amount", DFTU_TYPE_DECIMAL128, 0,
                              DFTU_TIME_UNIT_SECOND, NULL, 10, 2, 0);
}

typedef struct {
    int done;
} SrcCursor;

static dftu_task* src_next(void* self, int64_t max_rows,
                           dftu_result_frame* out) {
    SrcCursor* cur = (SrcCursor*)self;
    (void)max_rows;
    out->ok = 1;
    if (cur->done) {
        out->u.value = NULL;
        return NULL;
    }
    cur->done = 1;
    {
        static const int64_t n_vals[3] = {1, 2, 3};
        static const int64_t amount_vals[3] = {100, 200, 300};
        dftu_series* n_col =
            dftu_series_new_flat(DFTU_TYPE_INT64, n_vals, 3, NULL);
        dftu_series* amount_col =
            dftu_series_new_flat(DFTU_TYPE_INT64, amount_vals, 3, NULL);
        dftu_series* cols[2];
        cols[0] = n_col;
        cols[1] = amount_col;
        out->u.value = dftu_dataframe_new(SRC_NAMES, cols, 2);
    }
    return NULL;
}

static void src_cursor_destroy(void* self) { free(self); }

static const dftu_cursor_vt SRC_CURSOR_VT = {src_next, src_cursor_destroy, NULL,
                                             NULL, NULL};

static void* src_scan(void* self, const dftu_scan_request* req,
                      int32_t* out_pushed, void** out_cursor_self,
                      const dftu_cursor_vt** out_vt) {
    SrcCursor* cur;
    (void)self;
    (void)req;
    (void)out_pushed;
    cur = (SrcCursor*)calloc(1, sizeof(SrcCursor));
    *out_cursor_self = cur;
    *out_vt = &SRC_CURSOR_VT;
    return cur;
}

static void src_destroy(void* self) { (void)self; }

static const dftu_source_vt SRC_VT = {src_schema, src_scan, src_destroy,
                                      src_schema_types};

/* ---- Node: "reference_plugin.node", doubles column "n", passes "amount"
 * through unchanged.
 *
 * A node's open()/next() get no dftu_plugin_host*, so a node cannot reach
 * dftu_svc_coro to await its upstream, and plain C has no native coroutine
 * to co_await it with either. dftu_task_run (core/coro/abi.h) is the way
 * around that: it drives a task to completion on a given dftu_runtime and
 * blocks the calling thread, so next() can synchronously resolve its
 * upstream before transforming the frame. dftu_default_runtime() supplies
 * the runtime already driving this scan.
 */

typedef struct {
    void* in_self;
    const dftu_cursor_vt* in_vt;
} NodeCursor;

static dftu_task* node_next(void* self, int64_t max_rows,
                            dftu_result_frame* out) {
    NodeCursor* nc = (NodeCursor*)self;
    dftu_task* t = nc->in_vt->next(nc->in_self, max_rows, out);
    if (t && dftu_task_run(dftu_default_runtime(), t) != 0) {
        out->ok = 0;
        out->u.err =
            (dftu_error){0, 0, DFTU_COND_INTERNAL,
                         "reference_plugin.node: upstream task failed"};
        return NULL;
    }
    if (!DFTU_RESULT_OK(*out)) return NULL;
    {
        dftu_dataframe* frame = DFTU_RESULT_VALUE(*out);
        if (!frame) return NULL; /* end of stream */
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
                out->u.value = dftu_dataframe_new(SRC_NAMES, cols, 2);
            }
            free(doubled);
            dftu_series_free(n_col);
            dftu_dataframe_free(frame);
            out->ok = 1;
        }
    }
    return NULL;
}

static void node_cursor_destroy(void* self) {
    NodeCursor* nc = (NodeCursor*)self;
    if (nc->in_vt->destroy) nc->in_vt->destroy(nc->in_self);
    free(nc);
}

/* The node keeps every input column at its position (it rewrites `n` in
 * place), so a narrowing over its output is one over its input as well and
 * goes straight up; a node that dropped or reordered columns would remap or
 * refuse. The node holds nothing beyond the frame in flight, so it reports
 * no resident bytes and has nothing to reclaim. */
static dftu_task* node_narrow(void* self, const dftu_expr* predicate,
                              int32_t* out_applied) {
    NodeCursor* nc = (NodeCursor*)self;
    if (!nc->in_vt->narrow) {
        *out_applied = 0;
        return NULL;
    }
    return nc->in_vt->narrow(nc->in_self, predicate, out_applied);
}

static uint64_t node_bytes(void* self) {
    (void)self;
    return 0;
}

static uint64_t node_reclaim(void* self, uint64_t want) {
    (void)self;
    (void)want;
    return 0;
}

static const dftu_cursor_vt NODE_CURSOR_VT = {
    node_next, node_cursor_destroy, node_narrow, node_bytes, node_reclaim};

static void node_output_schema(void* self, const dftu_schema* in,
                               const dftu_op_arg* args, dftu_schema* out) {
    int32_t n, i;
    (void)self;
    (void)args;
    n = dftu_schema_field_count(in);
    for (i = 0; i < n; ++i) dftu_schema_copy_field(out, in, i);
}

static void* node_open(void* self, void* in_cursor_self,
                       const dftu_cursor_vt* in_vt, const dftu_op_arg* args,
                       void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    NodeCursor* nc;
    (void)self;
    (void)args;
    nc = (NodeCursor*)malloc(sizeof(NodeCursor));
    nc->in_self = in_cursor_self;
    nc->in_vt = in_vt;
    *out_cursor_self = nc;
    *out_vt = &NODE_CURSOR_VT;
    return nc;
}

static void node_destroy(void* self) { (void)self; }

static const dftu_node_vt NODE_VT = {node_output_schema, node_open,
                                     node_destroy};

/* ---- A registered op: "reference_plugin.count_rows" (series -> i64). */

static int64_t count_rows(const dftu_series* v) {
    return dftu_series_length(v);
}

static const dftu_op_desc COUNT_ROWS_OP = {"reference_plugin.count_rows",
                                           DFTU_OP_SIG(I64, SERIES, NONE, NONE),
                                           (const void*)&count_rows};

/* ---- Compose leaf op: doubles an int64 value, inline. */

static dftu_task* double_fn(void* state, const void* in, void* out, int* rc) {
    (void)state;
    *(int64_t*)out = *(const int64_t*)in * 2;
    *rc = 0;
    return NULL;
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

static void* dur_sum_init(void* self) {
    (void)self;
    return calloc(1, sizeof(DurSumState));
}

static int dur_sum_update(void* state, const dftu_dataframe* df,
                          dftu_error* err) {
    DurSumState* s = (DurSumState*)state;
    dftu_series* dur = dftu_dataframe_column(df, "dur");
    if (!dur) {
        if (err) err->message = "reference_plugin.dur_sum: no dur column";
        return -1;
    }
    {
        const int64_t rows = dftu_series_length(dur);
        const uint64_t* vals = (const uint64_t*)dftu_series_data(dur);
        int64_t i;
        for (i = 0; i < rows; ++i) s->sum += vals[i];
    }
    dftu_series_free(dur);
    return 0;
}

static int dur_sum_merge(void* into, void* other, dftu_error* err) {
    (void)err;
    ((DurSumState*)into)->sum += ((DurSumState*)other)->sum;
    return 0;
}

static uint64_t dur_sum_bytes(const void* state) {
    (void)state;
    return sizeof(DurSumState);
}

static void dur_sum_free_buf(void* data, void* ud) {
    (void)ud;
    free(data);
}

static int dur_sum_serialize(const void* state, dftu_bytes* out,
                             dftu_error* err) {
    uint64_t* buf;
    (void)err;
    buf = (uint64_t*)malloc(sizeof(uint64_t));
    *buf = ((const DurSumState*)state)->sum;
    out->data = buf;
    out->len = sizeof(uint64_t);
    out->free_fn = dur_sum_free_buf;
    out->ud = NULL;
    return 0;
}

static void* dur_sum_deserialize(void* self, dftu_bytes in, dftu_error* err) {
    DurSumState* s;
    (void)self;
    (void)err;
    s = (DurSumState*)calloc(1, sizeof(DurSumState));
    if (in.data && in.len == sizeof(uint64_t))
        memcpy(&s->sum, in.data, sizeof(uint64_t));
    return s;
}

static uint64_t g_dur_sum_result;

static int dur_sum_finalize(void* state, dftu_result_value* out,
                            dftu_error* err) {
    (void)err;
    g_dur_sum_result = ((DurSumState*)state)->sum;
    out->kind = DFTU_RESULT_KIND_BYTES;
    out->u.bytes.data = &g_dur_sum_result;
    out->u.bytes.len = sizeof(g_dur_sum_result);
    return 0;
}

static void dur_sum_destroy(void* state) { free(state); }

/* ---- coro run_blocking probe. */

static void bump_fn(void* arg) { *(int64_t*)arg += 1; }

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

static void trace_read_cb(const void* item, void* ud) {
    const dftu_dataframe* df = (const dftu_dataframe*)item;
    *(int64_t*)ud += dftu_dataframe_num_rows(df);
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

static void* make_slice(void* self) {
    RefSlice* s = (RefSlice*)calloc(1, sizeof(RefSlice));
    s->cfg = (const RefConfig*)self;
    return s;
}

static void merge(void* into, void* other) {
    RefSlice* a = (RefSlice*)into;
    RefSlice* b = (RefSlice*)other;
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
}

static void destroy_slice(void* slice) { free(slice); }

/* The slice's own footprint: the struct itself. The sketch and accumulator
   are host handles, measured and spilled by the host. Nothing here can be
   freed while the fold runs, so reclaim reports 0 honestly rather than
   pretending; the host then logs once that the budget is not honoured. */
static uint64_t slice_bytes(void* slice) {
    (void)slice;
    return (uint64_t)sizeof(RefSlice);
}

static uint64_t slice_reclaim(void* slice, uint64_t want) {
    (void)slice;
    (void)want;
    return 0;
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
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
                s->agg = agg->agg_new(host->h, "reference_plugin.agg",
                                      key_names, 1, specs, 1);
                g_status[REF_SVC_AGG] = s->agg ? REF_OK : REF_FAIL;
            }
        }

        {
            const dftu_svc_query* q =
                (const dftu_svc_query*)ref_get_service(host, REF_SVC_QUERY);
            if (q) {
                const char src[] = "cat == \"POSIX\"";
                dftu_query* compiled =
                    q->query_compile(host->h, src, (uint32_t)(sizeof(src) - 1));
                int matched =
                    compiled ? q->query_matches(host->h, compiled, df, 0) : -1;
                g_status[REF_SVC_QUERY] = matched == 1 ? REF_OK : REF_FAIL;
            }
        }

        {
            const dftu_svc_ops* ops =
                (const dftu_svc_ops*)ref_get_service(host, REF_SVC_OPS);
            if (ops) {
                dftu_series* cat = dftu_dataframe_column(df, "cat");
                int ops_ok;
                dftu_result_series hashed =
                    ops->run(host->h, "dftu.hash.fnv1a",
                             (const dftu_series* const*)&cat, 1, NULL);
                ops_ok = DFTU_RESULT_OK(hashed);
                if (ops_ok) dftu_series_free(DFTU_RESULT_VALUE(hashed));

                ops_ok =
                    ops_ok &&
                    (ops->find(host->h, "reference_plugin.count_rows") != NULL);

                {
                    dftu_result_scalar counted = ops->run_aggregate(
                        host->h, "reference_plugin.count_rows",
                        (const dftu_series* const*)&cat, 1, NULL);
                    ops_ok = ops_ok && DFTU_RESULT_OK(counted) &&
                             DFTU_RESULT_VALUE(counted).value.i == rows;
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
                int rc = pr->register_provider(
                    host->h, "reference_plugin.rejected", &SRC_VT, NULL);
                g_status[REF_SVC_PROVIDERS] = (rc != 0) ? REF_OK : REF_FAIL;
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
        agg->agg_accumulate(host->h, s->agg, df);
    }

    return NULL;
}

/* ---- on_finalize: the async services (io, writer, compose) need something
   to sequence them, which C has no native coroutine for, so
   dftu_svc_coro::drive plays that role - it calls `step` after every task it
   awaits, and `step` treats `state` as a program counter. */

typedef struct {
    int state;
    const dftu_plugin_host* host;
    RefSlice* slice;

    int fd;
    int64_t io_wn, io_rn;
    int io_rc;
    char io_buf[32];

    dftu_writer* writer;

    dftu_op* op;
    int64_t compose_in, compose_out;
    int compose_rc;
} FinalizeCtx;

static void finalize_tail(FinalizeCtx* ctx) {
    const dftu_plugin_host* host = ctx->host;
    const char* workdir = ctx->slice->cfg->workdir;

    /* coro: run_blocking is the one synchronous slot. */
    {
        const dftu_svc_coro* coro =
            (const dftu_svc_coro*)ref_get_service(host, REF_SVC_CORO);
        if (coro) {
            int64_t counter = 0;
            coro->run_blocking(host->h, bump_fn, &counter);
            g_status[REF_SVC_CORO] = (counter == 1) ? REF_OK : REF_FAIL;
        }
    }

    /* trace: write TRACE_N events, close, then read them back. */
    {
        const dftu_svc_trace* tr =
            (const dftu_svc_trace*)ref_get_service(host, REF_SVC_TRACE);
        if (tr) {
            char path[600];
            dftu_trace_writer* tw;
            int trace_ok = 0;
            snprintf(path, sizeof(path), "%s/reference_trace.pfw.gz", workdir);
            tw = tr->trace_open_write(host->h, path);
            if (tw) {
                dftu_dataframe* frame = build_trace_frame();
                int wrc = tr->trace_write(host->h, tw, frame);
                int crc;
                dftu_dataframe_free(frame);
                crc = tr->trace_close(host->h, tw);
                if (wrc == 0 && crc == 0) {
                    int64_t read_rows = 0;
                    int rrc = tr->trace_read(host->h, path, trace_read_cb,
                                             &read_rows);
                    trace_ok = (rrc == 0) && (read_rows == TRACE_N);
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
                int wrc;
                snprintf(path, sizeof(path), "%s/reference_arrow.ipc", workdir);
                wrc = ar->arrow_write_ipc(host->h, &array, &schema, path);
                if (wrc == 0) {
                    struct ArrowArray in_array;
                    struct ArrowSchema in_schema;
                    int rrc;
                    memset(&in_array, 0, sizeof(in_array));
                    memset(&in_schema, 0, sizeof(in_schema));
                    rrc = ar->arrow_read_ipc(host->h, path, &in_array,
                                             &in_schema);
                    if (rrc == 0) {
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
            if (names) dftu_query_string_free(names);

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
        if (agg && ctx->slice->agg) {
            dftu_dataframe* result =
                agg->agg_result(host->h, "reference_plugin.agg");
            int agg_ok = result && dftu_dataframe_num_rows(result) >= 1;
            if (result) dftu_dataframe_free(result);
            if (g_status[REF_SVC_AGG] == REF_OK)
                g_status[REF_SVC_AGG] = agg_ok ? REF_OK : REF_FAIL;
        }
    }
    {
        const dftu_svc_sketch* sk =
            (const dftu_svc_sketch*)ref_get_service(host, REF_SVC_SKETCH);
        if (sk && ctx->slice->sketch) {
            dftu_quantiles q = sk->sketch_result(host->h, ctx->slice->sketch);
            int sketch_ok =
                q.count > 0 && q.count == (uint64_t)ctx->slice->rows_seen;
            sk->sketch_free(host->h, ctx->slice->sketch);
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
            res->emit(host->h, "reference_ok", &v);

            memset(&v, 0, sizeof(v));
            v.kind = DFTU_RESULT_KIND_BYTES;
            v.u.bytes.data = report;
            v.u.bytes.len = (uint64_t)off;
            res->emit(host->h, "reference_report", &v);
        }
    }
}

static dftu_task* finalize_step(void* self) {
    FinalizeCtx* ctx = (FinalizeCtx*)self;
    const dftu_plugin_host* host = ctx->host;
    const char* workdir = ctx->slice->cfg->workdir;
    char path[600];

    switch (ctx->state++) {
        case 0: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            if (!io) return NULL;
            snprintf(path, sizeof(path), "%s/reference_io.bin", workdir);
            return io->open(host->h, path, O_CREAT | O_RDWR | O_TRUNC, 0644,
                            &ctx->fd);
        }
        case 1: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            static const char data[] = "reference-io";
            if (!io || ctx->fd < 0) return NULL;
            return io->write(host->h, ctx->fd, data, sizeof(data), &ctx->io_wn);
        }
        case 2: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            if (!io || ctx->fd < 0) return NULL;
            return io->pread(host->h, ctx->fd, ctx->io_buf, sizeof(ctx->io_buf),
                             0, &ctx->io_rn);
        }
        case 3: {
            const dftu_io* io =
                (const dftu_io*)ref_get_service(host, REF_SVC_IO);
            if (!io || ctx->fd < 0) return NULL;
            return io->close(host->h, ctx->fd, &ctx->io_rc);
        }
        case 4: {
            static const char data[] = "reference-io";
            const dftu_svc_writer* w;
            int io_ok = ctx->fd >= 0 && ctx->io_wn == (int64_t)sizeof(data) &&
                        ctx->io_rn == (int64_t)sizeof(data) &&
                        memcmp(ctx->io_buf, data, sizeof(data)) == 0 &&
                        ctx->io_rc == 0;
            g_status[REF_SVC_IO] = io_ok ? REF_OK : REF_FAIL;

            w = (const dftu_svc_writer*)ref_get_service(host, REF_SVC_WRITER);
            if (!w) return NULL;
            snprintf(path, sizeof(path), "%s/reference_writer.out", workdir);
            ctx->writer = w->writer_create(host->h, path, 1, 0);
            if (!ctx->writer) {
                g_status[REF_SVC_WRITER] = REF_FAIL;
                return NULL;
            }
            return w->writer_open(host->h, ctx->writer);
        }
        case 5: {
            const dftu_svc_writer* w =
                (const dftu_svc_writer*)ref_get_service(host, REF_SVC_WRITER);
            static const char chunk[] = "reference-writer-chunk";
            if (!w || !ctx->writer) return NULL;
            return w->writer_chunk(host->h, ctx->writer, 0, chunk,
                                   sizeof(chunk));
        }
        case 6: {
            const dftu_svc_writer* w =
                (const dftu_svc_writer*)ref_get_service(host, REF_SVC_WRITER);
            if (!w || !ctx->writer) return NULL;
            return w->writer_close(host->h, ctx->writer);
        }
        case 7: {
            const dftu_svc_compose* c;
            struct stat st;
            snprintf(path, sizeof(path), "%s/reference_writer.out.shard_0",
                     workdir);
            g_status[REF_SVC_WRITER] =
                (ctx->writer && stat(path, &st) == 0 && st.st_size > 0)
                    ? REF_OK
                    : REF_FAIL;

            c = (const dftu_svc_compose*)ref_get_service(host, REF_SVC_COMPOSE);
            if (!c) return NULL;
            ctx->op = c->make_op(host->h, double_fn, NULL, NULL, DFTU_T_I64,
                                 sizeof(int64_t), DFTU_T_I64, sizeof(int64_t));
            if (!ctx->op) {
                g_status[REF_SVC_COMPOSE] = REF_FAIL;
                return NULL;
            }
            ctx->compose_in = 21;
            return c->run(host->h, ctx->op, &ctx->compose_in, &ctx->compose_out,
                          &ctx->compose_rc);
        }
        default: {
            const dftu_svc_compose* c =
                (const dftu_svc_compose*)ref_get_service(host, REF_SVC_COMPOSE);
            int compose_ok = ctx->op && ctx->compose_rc == 0 &&
                             ctx->compose_out == ctx->compose_in * 2;
            g_status[REF_SVC_COMPOSE] = compose_ok ? REF_OK : REF_FAIL;
            if (c && ctx->op) c->free_op(host->h, ctx->op);

            finalize_tail(ctx);
            free(ctx);
            return NULL;
        }
    }
}

static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
    RefSlice* s = (RefSlice*)slice;
    const dftu_svc_coro* coro;
    FinalizeCtx* ctx = (FinalizeCtx*)calloc(1, sizeof(FinalizeCtx));
    ctx->host = host;
    ctx->slice = s;

    coro = (const dftu_svc_coro*)ref_get_service(host, REF_SVC_CORO);
    if (!coro) {
        free(ctx);
        return NULL;
    }
    return coro->drive(host->h, finalize_step, ctx);
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

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                const dftu_value* config) {
    RefConfig* cfg;
    const dftu_value* workdir_v;
    const char* workdir_s;
    uint32_t workdir_len = 0;
    const dftu_svc_ops* ops;
    const dftu_svc_providers* providers;

    workdir_v = dftu_obj_get(config, "workdir");
    workdir_s = dftu_as_str(workdir_v, &workdir_len);
    if (!workdir_s || workdir_len == 0 || workdir_len >= 500) return NULL;

    cfg = (RefConfig*)calloc(1, sizeof(RefConfig));
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
    if (ops && ops->register_op) ops->register_op(h->h, &COUNT_ROWS_OP);

    providers =
        (const dftu_svc_providers*)h->get_service(h->h, DFTU_SVC_PROVIDERS);
    if (providers && providers->register_provider)
        providers->register_provider(h->h, "reference_plugin.src", &SRC_VT,
                                     NULL);

    /* register_state is callable only from the factory: by run time the
       registry the host plans from is settled. */
    {
        const dftu_svc_agg* agg_reg =
            (const dftu_svc_agg*)ref_get_service(h, REF_SVC_AGG);
        if (agg_reg && agg_reg->register_state) {
            dftu_state_desc desc;
            memset(&desc, 0, sizeof(desc));
            desc.name = DUR_SUM_STATE_NAME;
            desc.init = dur_sum_init;
            desc.update = dur_sum_update;
            desc.merge = dur_sum_merge;
            desc.bytes = dur_sum_bytes;
            desc.serialize = dur_sum_serialize;
            desc.deserialize = dur_sum_deserialize;
            desc.finalize = dur_sum_finalize;
            desc.destroy = dur_sum_destroy;
            g_state_ok = agg_reg->register_state(h->h, &desc, NULL) == 0
                             ? REF_OK
                             : REF_FAIL;
        } else if (!agg_reg) {
            g_state_ok = REF_MISSING;
        } else {
            g_state_ok = REF_FAIL;
        }
    }

    dftu_node_register("reference_plugin.node", &NODE_VT, NULL);

    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.self = cfg;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.config_keys = config_keys;
    g_plugin.reads = reads;
    g_plugin.bytes = slice_bytes;
    g_plugin.reclaim = slice_reclaim;
    return &g_plugin;
}
