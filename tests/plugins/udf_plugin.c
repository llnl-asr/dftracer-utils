// SPDX-License-Identifier: MIT
/* Test-only plugin whose factory registers scalar UDFs:
 *   uadd(a, b)       two numeric columns; the result type follows the types
 *   uover(x, limit)  Bool predicate; `limit` must be a constant
 *   uplus1(x)        x + 1, default null handling (the host masks nulls)
 *   unull(x)         NULL_AWARE: -1 where x is null, else x
 *   ufail(x)         exec fails with a message
 *   uslow(x)         x, in steps that sleep and poll for a cancel
 *   uvol(x)          VOLATILE: x
 *   ubadrows(x)      exec returns one row too many
 *   ubadtype(x)      exec returns Float64 though bind chose Int64
 *   ustr(s)          String in, String out: s with "!" appended
 * It counts binds, frees and slow steps, which the test reads by dlsym. */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int64_t g_binds;
static int64_t g_frees;
static int64_t g_slow_steps;

DFTU_PLUGIN_EXPORT int64_t udf_plugin_binds(void) { return g_binds; }
DFTU_PLUGIN_EXPORT int64_t udf_plugin_frees(void) { return g_frees; }
DFTU_PLUGIN_EXPORT int64_t udf_plugin_slow_steps(void) { return g_slow_steps; }

typedef struct {
    dftu_dtype t[2];
    dftu_dtype out;
    double limit;
} Bound;

static int numeric(dftu_dtype t) {
    return t == DFTU_TYPE_INT64 || t == DFTU_TYPE_UINT64 ||
           t == DFTU_TYPE_FLOAT64 || t == DFTU_TYPE_INT32 ||
           t == DFTU_TYPE_UINT32;
}

static double at(const dftu_series* s, dftu_dtype t, int64_t i) {
    const void* d = dftu_series_data(s);
    switch (t) {
        case DFTU_TYPE_INT64:
            return (double)((const int64_t*)d)[i];
        case DFTU_TYPE_UINT64:
            return (double)((const uint64_t*)d)[i];
        case DFTU_TYPE_INT32:
            return (double)((const int32_t*)d)[i];
        case DFTU_TYPE_UINT32:
            return (double)((const uint32_t*)d)[i];
        default:
            return ((const double*)d)[i];
    }
}

static dftu_status bind_common(dftu_ctx* ctx, const dftu_type_info* args,
                               uint32_t n, dftu_dtype out_type, Bound** out) {
    uint32_t i;
    Bound* b = (Bound*)calloc(1, sizeof(Bound));
    if (!b) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    for (i = 0; i < n; ++i) {
        if (!numeric(args[i].id)) {
            free(b);
            return dftu_fail(ctx, DFTU_E_INVALID,
                             "udf_plugin: an argument is not a number");
        }
        b->t[i] = args[i].id;
    }
    b->out = out_type;
    ++g_binds;
    *out = b;
    return DFTU_OK;
}

static void free_bound(void* bound) {
    ++g_frees;
    free(bound);
}

static dftu_status set_out(dftu_type_info* out, dftu_dtype id) {
    out->struct_size = sizeof(*out);
    out->id = id;
    return DFTU_OK;
}

static dftu_series* new_i64(const int64_t* v, uint64_t rows) {
    return dftu_series_new_flat(DFTU_TYPE_INT64, v, (int64_t)rows, NULL);
}

static dftu_status uadd_bind(dftu_ctx* ctx, void* self,
                             const dftu_type_info* args, uint32_t n,
                             const dftu_scalar* const* consts,
                             dftu_type_info* out, void** bound) {
    Bound* b = NULL;
    dftu_dtype type = DFTU_TYPE_INT64;
    dftu_status st;
    (void)self;
    (void)consts;
    if (n == 2 &&
        (args[0].id == DFTU_TYPE_FLOAT64 || args[1].id == DFTU_TYPE_FLOAT64))
        type = DFTU_TYPE_FLOAT64;
    st = bind_common(ctx, args, n, type, &b);
    if (st < 0) return st;
    *bound = b;
    return set_out(out, type);
}

static dftu_status uadd_exec(dftu_ctx* ctx, void* bound,
                             const dftu_series* const* args, uint32_t n,
                             uint64_t rows, dftu_series** out) {
    const Bound* b = (const Bound*)bound;
    uint64_t i;
    (void)n;
    (void)ctx;
    if (b->out == DFTU_TYPE_FLOAT64) {
        double* v = (double*)malloc(rows * sizeof(double) + 1);
        for (i = 0; i < rows; ++i)
            v[i] = at(args[0], b->t[0], (int64_t)i) +
                   at(args[1], b->t[1], (int64_t)i);
        *out = dftu_series_new_flat(DFTU_TYPE_FLOAT64, v, (int64_t)rows, NULL);
        free(v);
    } else {
        int64_t* v = (int64_t*)malloc(rows * sizeof(int64_t) + 1);
        for (i = 0; i < rows; ++i)
            v[i] = (int64_t)(at(args[0], b->t[0], (int64_t)i) +
                             at(args[1], b->t[1], (int64_t)i));
        *out = new_i64(v, rows);
        free(v);
    }
    return DFTU_OK;
}

static dftu_status uover_bind(dftu_ctx* ctx, void* self,
                              const dftu_type_info* args, uint32_t n,
                              const dftu_scalar* const* consts,
                              dftu_type_info* out, void** bound) {
    Bound* b = NULL;
    dftu_status st;
    (void)self;
    if (!consts[1] || consts[1]->kind == DFTU_SCALAR_TAG_STR)
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "uover: the limit must be a constant number");
    st = bind_common(ctx, args, n, DFTU_TYPE_BOOL, &b);
    if (st < 0) return st;
    b->limit = consts[1]->kind == DFTU_SCALAR_TAG_F64 ? consts[1]->value.d
               : consts[1]->kind == DFTU_SCALAR_TAG_U64
                   ? (double)consts[1]->value.u
                   : (double)consts[1]->value.i;
    *bound = b;
    return set_out(out, DFTU_TYPE_BOOL);
}

static dftu_status uover_exec(dftu_ctx* ctx, void* bound,
                              const dftu_series* const* args, uint32_t n,
                              uint64_t rows, dftu_series** out) {
    const Bound* b = (const Bound*)bound;
    uint8_t* bits = (uint8_t*)calloc((rows + 7) / 8 + 1, 1);
    uint64_t i;
    (void)ctx;
    (void)n;
    for (i = 0; i < rows; ++i)
        if (at(args[0], b->t[0], (int64_t)i) > b->limit)
            bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    *out = dftu_series_new_flat(DFTU_TYPE_BOOL, bits, (int64_t)rows, NULL);
    free(bits);
    return DFTU_OK;
}

static dftu_status unary_bind(dftu_ctx* ctx, void* self,
                              const dftu_type_info* args, uint32_t n,
                              const dftu_scalar* const* consts,
                              dftu_type_info* out, void** bound) {
    Bound* b = NULL;
    dftu_status st;
    (void)self;
    (void)consts;
    st = bind_common(ctx, args, n, DFTU_TYPE_INT64, &b);
    if (st < 0) return st;
    *bound = b;
    return set_out(out, DFTU_TYPE_INT64);
}

static dftu_status uplus1_exec(dftu_ctx* ctx, void* bound,
                               const dftu_series* const* args, uint32_t n,
                               uint64_t rows, dftu_series** out) {
    const Bound* b = (const Bound*)bound;
    int64_t* v = (int64_t*)malloc(rows * sizeof(int64_t) + 1);
    uint64_t i;
    (void)ctx;
    (void)n;
    for (i = 0; i < rows; ++i)
        v[i] = (int64_t)at(args[0], b->t[0], (int64_t)i) + 1;
    *out = new_i64(v, rows);
    free(v);
    return DFTU_OK;
}

static dftu_status unull_exec(dftu_ctx* ctx, void* bound,
                              const dftu_series* const* args, uint32_t n,
                              uint64_t rows, dftu_series** out) {
    const Bound* b = (const Bound*)bound;
    int64_t* v = (int64_t*)malloc(rows * sizeof(int64_t) + 1);
    uint64_t i;
    (void)ctx;
    (void)n;
    for (i = 0; i < rows; ++i)
        v[i] = dftu_series_is_null(args[0], (int64_t)i)
                   ? -1
                   : (int64_t)at(args[0], b->t[0], (int64_t)i);
    *out = new_i64(v, rows);
    free(v);
    return DFTU_OK;
}

static dftu_status ufail_exec(dftu_ctx* ctx, void* bound,
                              const dftu_series* const* args, uint32_t n,
                              uint64_t rows, dftu_series** out) {
    (void)bound;
    (void)args;
    (void)n;
    (void)rows;
    (void)out;
    return dftu_fail(ctx, DFTU_E_INVALID, "ufail: refused on purpose");
}

#define SLOW_STEP_ROWS 4
#define SLOW_STEP_USEC 10000

static dftu_status uslow_exec(dftu_ctx* ctx, void* bound,
                              const dftu_series* const* args, uint32_t n,
                              uint64_t rows, dftu_series** out) {
    const Bound* b = (const Bound*)bound;
    int64_t* v = (int64_t*)malloc(rows * sizeof(int64_t) + 1);
    uint64_t i;
    (void)n;
    for (i = 0; i < rows; ++i) {
        if (i % SLOW_STEP_ROWS == 0) {
            const dftu_status st = dftu_check(ctx);
            if (st < 0) {
                free(v);
                return st;
            }
            usleep(SLOW_STEP_USEC);
            ++g_slow_steps;
        }
        v[i] = (int64_t)at(args[0], b->t[0], (int64_t)i);
    }
    *out = new_i64(v, rows);
    free(v);
    return DFTU_OK;
}

static dftu_status uident_exec(dftu_ctx* ctx, void* bound,
                               const dftu_series* const* args, uint32_t n,
                               uint64_t rows, dftu_series** out) {
    return uplus1_exec(ctx, bound, args, n, rows, out);
}

static dftu_status ubadrows_exec(dftu_ctx* ctx, void* bound,
                                 const dftu_series* const* args, uint32_t n,
                                 uint64_t rows, dftu_series** out) {
    int64_t* v = (int64_t*)calloc(rows + 1, sizeof(int64_t));
    (void)ctx;
    (void)bound;
    (void)args;
    (void)n;
    *out = new_i64(v, rows + 1);
    free(v);
    return DFTU_OK;
}

static dftu_status ubadtype_exec(dftu_ctx* ctx, void* bound,
                                 const dftu_series* const* args, uint32_t n,
                                 uint64_t rows, dftu_series** out) {
    double* v = (double*)malloc(rows * sizeof(double) + 1);
    uint64_t i;
    (void)ctx;
    (void)bound;
    (void)args;
    (void)n;
    for (i = 0; i < rows; ++i) v[i] = 0.5;
    *out = dftu_series_new_flat(DFTU_TYPE_FLOAT64, v, (int64_t)rows, NULL);
    free(v);
    return DFTU_OK;
}

static dftu_status ustr_bind(dftu_ctx* ctx, void* self,
                             const dftu_type_info* args, uint32_t n,
                             const dftu_scalar* const* consts,
                             dftu_type_info* out, void** bound) {
    Bound* b = (Bound*)calloc(1, sizeof(Bound));
    (void)self;
    (void)consts;
    if (!b) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    if (n != 1 || args[0].id != DFTU_TYPE_STRING) {
        free(b);
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "ustr: the argument is not a string");
    }
    ++g_binds;
    *bound = b;
    return set_out(out, DFTU_TYPE_STRING);
}

static dftu_status ustr_exec(dftu_ctx* ctx, void* bound,
                             const dftu_series* const* args, uint32_t n,
                             uint64_t rows, dftu_series** out) {
    uint64_t i;
    size_t used = 0, cap = 1;
    int32_t* offsets = (int32_t*)malloc((rows + 1) * sizeof(int32_t));
    char* data;
    (void)ctx;
    (void)bound;
    (void)n;
    for (i = 0; i < rows; ++i) {
        int64_t len = 0;
        dftu_series_string_at(args[0], (int64_t)i, &len);
        cap += (size_t)len + 1;
    }
    data = (char*)malloc(cap);
    for (i = 0; i < rows; ++i) {
        int64_t len = 0;
        const char* s = dftu_series_string_at(args[0], (int64_t)i, &len);
        offsets[i] = (int32_t)used;
        if (s) memcpy(data + used, s, (size_t)len);
        used += (size_t)len;
        data[used++] = '!';
    }
    offsets[rows] = (int32_t)used;
    *out = dftu_series_new_string(DFTU_TYPE_STRING, offsets, data,
                                  (int64_t)rows, NULL);
    free(offsets);
    free(data);
    return DFTU_OK;
}

static const dftu_udf_desc UDFS[] = {
    {sizeof(dftu_udf_desc), "uadd", 0, 2, 2, uadd_bind, uadd_exec, free_bound},
    {sizeof(dftu_udf_desc), "uover", 0, 2, 2, uover_bind, uover_exec,
     free_bound},
    {sizeof(dftu_udf_desc), "uplus1", 0, 1, 1, unary_bind, uplus1_exec,
     free_bound},
    {sizeof(dftu_udf_desc), "unull", DFTU_FN_NULL_AWARE, 1, 1, unary_bind,
     unull_exec, free_bound},
    {sizeof(dftu_udf_desc), "ufail", 0, 1, 1, unary_bind, ufail_exec,
     free_bound},
    {sizeof(dftu_udf_desc), "uslow", 0, 1, 1, unary_bind, uslow_exec,
     free_bound},
    {sizeof(dftu_udf_desc), "uvol", DFTU_FN_VOLATILE, 1, 1, unary_bind,
     uident_exec, free_bound},
    {sizeof(dftu_udf_desc), "ubadrows", 0, 1, 1, unary_bind, ubadrows_exec,
     free_bound},
    {sizeof(dftu_udf_desc), "ubadtype", 0, 1, 1, unary_bind, ubadtype_exec,
     free_bound},
    {sizeof(dftu_udf_desc), "ustr", 0, 1, 1, ustr_bind, ustr_exec, free_bound},
};

static dftu_plugin g_plugin;

static void destroy(void* self) { (void)self; }

DFTU_PLUGIN_INFO("udf_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_ops* ops;
    size_t i;
    (void)config;
    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (!ops || !ops->register_udf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udf registry at load");
    for (i = 0; i < sizeof(UDFS) / sizeof(UDFS[0]); ++i) {
        const dftu_status st = ops->register_udf(ctx, h->h, &UDFS[i], NULL);
        if (st < 0) return st;
    }
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
