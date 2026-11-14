// SPDX-License-Identifier: MIT
/* Test-only plugin whose factory registers user-defined aggregates:
 *   uwsum(x, w)   POD state: Int64 sum of x * w over Int64 arguments
 *   ucount()      POD state, no argument: the number of rows
 *   usum(x)       POD state: Int64 sum of integers, Float64 sum of floats
 *   udistinct(x)  non-POD state (a heap set): the count of distinct Int64
 *   ucountall(x)  NULL_AWARE POD state: the number of rows, nulls included
 *   ufail(x)      update fails with a message
 *   ubadrows(x)   finalize returns one row too many
 *   ualign32(x)   POD state of 24 bytes aligned to 32; counts misaligned inits
 *   ualign16(x)   POD state of 40 bytes aligned to 16; counts misaligned inits
 *   ufailcombine(x)  combine fails with a message
 *   usumr(x)      usum with retract and value, so a window slides its frame
 *   uretfail(x)   usumr whose retract fails with a message
 *   uwneg(x)      usumr with a UInt64 result whose value() gives -1
 *   uwbig(x)      usumr with an Int64 result whose value() gives UINT64_MAX
 *   userial(x)    usum with a non-POD state and without
 *                 DFTU_AGG_PARALLEL_SAFE; every callback counts the threads
 *                 inside the plugin at once
 * It counts binds, frees, inits and destroys, which the test reads by dlsym. */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DFTU_PLUGIN_EXPORT int64_t udaf_plugin_binds(void);
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_frees(void);
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_inits(void);
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_destroys(void);
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_misaligned(void);
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_serial_serializes(void);
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_max_active(void);
DFTU_PLUGIN_EXPORT void udaf_plugin_reset_active(void);

static int64_t g_binds;
static int64_t g_frees;
static int64_t g_inits;
static int64_t g_destroys;
static int64_t g_misaligned;
static atomic_int g_serial_serializes;
static atomic_int g_active;
static atomic_int g_max_active;

DFTU_PLUGIN_EXPORT int64_t udaf_plugin_binds(void) { return g_binds; }
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_frees(void) { return g_frees; }
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_inits(void) { return g_inits; }
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_destroys(void) { return g_destroys; }
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_misaligned(void) { return g_misaligned; }
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_serial_serializes(void) {
    return atomic_load(&g_serial_serializes);
}
DFTU_PLUGIN_EXPORT int64_t udaf_plugin_max_active(void) {
    return atomic_load(&g_max_active);
}
DFTU_PLUGIN_EXPORT void udaf_plugin_reset_active(void) {
    atomic_store(&g_max_active, 0);
}

typedef struct {
    dftu_dtype in;
    dftu_dtype out;
} Bound;

static int is_int(dftu_dtype t) {
    return t == DFTU_TYPE_INT64 || t == DFTU_TYPE_UINT64 ||
           t == DFTU_TYPE_INT32 || t == DFTU_TYPE_UINT32;
}

static int64_t int_at(const dftu_series* s, dftu_dtype t, uint64_t i) {
    const void* d = dftu_series_data(s);
    switch (t) {
        case DFTU_TYPE_INT64:
            return ((const int64_t*)d)[i];
        case DFTU_TYPE_UINT64:
            return (int64_t)((const uint64_t*)d)[i];
        case DFTU_TYPE_INT32:
            return ((const int32_t*)d)[i];
        default:
            return ((const uint32_t*)d)[i];
    }
}

static dftu_status bind_as(dftu_ctx* ctx, const dftu_type_info* args,
                           uint32_t n, dftu_type_info* out, void** bound,
                           dftu_dtype result, int ints_only, int i64_only) {
    Bound* b;
    if (n != 1 || (i64_only && args[0].id != DFTU_TYPE_INT64) ||
        (ints_only && !is_int(args[0].id) && args[0].id != DFTU_TYPE_FLOAT64))
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "udaf_plugin: the argument type is not accepted");
    b = (Bound*)calloc(1, sizeof(Bound));
    if (!b) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    b->in = args[0].id;
    b->out = result;
    ++g_binds;
    *bound = b;
    out->struct_size = sizeof(*out);
    out->id = result;
    return DFTU_OK;
}

static void free_bound(void* bound) {
    ++g_frees;
    free(bound);
}

/* usum */

typedef struct {
    int64_t i;
    double d;
} Sum;

static dftu_status usum_bind(dftu_ctx* ctx, void* self,
                             const dftu_type_info* args, uint32_t n,
                             const dftu_scalar* const* consts,
                             dftu_type_info* out, void** bound) {
    (void)self;
    (void)consts;
    return bind_as(ctx, args, n, out, bound,
                   n == 1 && args[0].id == DFTU_TYPE_FLOAT64 ? DFTU_TYPE_FLOAT64
                                                             : DFTU_TYPE_INT64,
                   1, 0);
}

static void count_inits(void* bound, void* const* states, uint64_t n) {
    (void)bound;
    (void)states;
    g_inits += (int64_t)n;
}

static dftu_status usum_update(dftu_ctx* ctx, void* bound, void* const* states,
                               const dftu_series* const* args, uint32_t nargs,
                               uint64_t rows) {
    const Bound* b = (const Bound*)bound;
    uint64_t i;
    (void)ctx;
    (void)nargs;
    for (i = 0; i < rows; ++i) {
        Sum* s = (Sum*)states[i];
        if (!s) continue;
        if (b->in == DFTU_TYPE_FLOAT64)
            s->d += ((const double*)dftu_series_data(args[0]))[i];
        else
            s->i += int_at(args[0], b->in, i);
    }
    return DFTU_OK;
}

static dftu_status usum_combine(dftu_ctx* ctx, void* bound, void* const* into,
                                void* const* from, uint64_t n) {
    uint64_t i;
    (void)ctx;
    (void)bound;
    for (i = 0; i < n; ++i) {
        ((Sum*)into[i])->i += ((const Sum*)from[i])->i;
        ((Sum*)into[i])->d += ((const Sum*)from[i])->d;
    }
    return DFTU_OK;
}

static dftu_status usum_finalize(dftu_ctx* ctx, void* bound,
                                 void* const* states, uint64_t n,
                                 dftu_series** out) {
    const Bound* b = (const Bound*)bound;
    uint64_t i;
    (void)ctx;
    if (b->out == DFTU_TYPE_FLOAT64) {
        double* v = (double*)malloc(n * sizeof(double) + 1);
        for (i = 0; i < n; ++i) v[i] = ((const Sum*)states[i])->d;
        *out = dftu_series_new_flat(DFTU_TYPE_FLOAT64, v, (int64_t)n, NULL);
        free(v);
    } else {
        int64_t* v = (int64_t*)malloc(n * sizeof(int64_t) + 1);
        for (i = 0; i < n; ++i) v[i] = ((const Sum*)states[i])->i;
        *out = dftu_series_new_flat(DFTU_TYPE_INT64, v, (int64_t)n, NULL);
        free(v);
    }
    return DFTU_OK;
}

static dftu_status usumr_retract(dftu_ctx* ctx, void* bound,
                                 void* const* states,
                                 const dftu_series* const* args, uint32_t nargs,
                                 uint64_t rows) {
    const Bound* b = (const Bound*)bound;
    uint64_t i;
    (void)ctx;
    (void)nargs;
    for (i = 0; i < rows; ++i) {
        Sum* s = (Sum*)states[i];
        if (!s) continue;
        if (b->in == DFTU_TYPE_FLOAT64)
            s->d -= ((const double*)dftu_series_data(args[0]))[i];
        else
            s->i -= int_at(args[0], b->in, i);
    }
    return DFTU_OK;
}

static dftu_status uretfail_retract(dftu_ctx* ctx, void* bound,
                                    void* const* states,
                                    const dftu_series* const* args,
                                    uint32_t nargs, uint64_t rows) {
    (void)bound;
    (void)states;
    (void)args;
    (void)nargs;
    (void)rows;
    return dftu_fail(ctx, DFTU_E_INVALID, "uretfail: retract refused");
}

static dftu_status usumr_value(dftu_ctx* ctx, void* bound, const void* state,
                               dftu_scalar* out) {
    const Bound* b = (const Bound*)bound;
    (void)ctx;
    memset(out, 0, sizeof(*out));
    if (b->out == DFTU_TYPE_FLOAT64) {
        out->kind = DFTU_SCALAR_TAG_F64;
        out->value.d = ((const Sum*)state)->d;
    } else {
        out->kind = DFTU_SCALAR_TAG_I64;
        out->value.i = ((const Sum*)state)->i;
    }
    return DFTU_OK;
}

static dftu_status uwneg_bind(dftu_ctx* ctx, void* self,
                              const dftu_type_info* args, uint32_t n,
                              const dftu_scalar* const* consts,
                              dftu_type_info* out, void** bound) {
    (void)self;
    (void)consts;
    return bind_as(ctx, args, n, out, bound, DFTU_TYPE_UINT64, 1, 0);
}

static dftu_status uwneg_value(dftu_ctx* ctx, void* bound, const void* state,
                               dftu_scalar* out) {
    (void)ctx;
    (void)bound;
    (void)state;
    memset(out, 0, sizeof(*out));
    out->kind = DFTU_SCALAR_TAG_I64;
    out->value.i = -1;
    return DFTU_OK;
}

static dftu_status uwbig_bind(dftu_ctx* ctx, void* self,
                              const dftu_type_info* args, uint32_t n,
                              const dftu_scalar* const* consts,
                              dftu_type_info* out, void** bound) {
    (void)self;
    (void)consts;
    return bind_as(ctx, args, n, out, bound, DFTU_TYPE_INT64, 1, 0);
}

static dftu_status uwbig_value(dftu_ctx* ctx, void* bound, const void* state,
                               dftu_scalar* out) {
    (void)ctx;
    (void)bound;
    (void)state;
    memset(out, 0, sizeof(*out));
    out->kind = DFTU_SCALAR_TAG_U64;
    out->value.u = UINT64_MAX;
    return DFTU_OK;
}

/* userial: usum that records how many threads are inside it at once */

static void enter_serial(void) {
    const int now = atomic_fetch_add(&g_active, 1) + 1;
    int seen = atomic_load(&g_max_active);
    while (now > seen &&
           !atomic_compare_exchange_weak(&g_max_active, &seen, now));
    usleep(200);
}

static void leave_serial(void) { atomic_fetch_sub(&g_active, 1); }

static dftu_status userial_bind(dftu_ctx* ctx, void* self,
                                const dftu_type_info* args, uint32_t n,
                                const dftu_scalar* const* consts,
                                dftu_type_info* out, void** bound) {
    dftu_status st;
    enter_serial();
    st = usum_bind(ctx, self, args, n, consts, out, bound);
    leave_serial();
    return st;
}

static void userial_init(void* bound, void* const* states, uint64_t n) {
    enter_serial();
    count_inits(bound, states, n);
    leave_serial();
}

static dftu_status userial_update(dftu_ctx* ctx, void* bound,
                                  void* const* states,
                                  const dftu_series* const* args,
                                  uint32_t nargs, uint64_t rows) {
    dftu_status st;
    enter_serial();
    st = usum_update(ctx, bound, states, args, nargs, rows);
    leave_serial();
    return st;
}

static dftu_status userial_combine(dftu_ctx* ctx, void* bound,
                                   void* const* into, void* const* from,
                                   uint64_t n) {
    dftu_status st;
    enter_serial();
    st = usum_combine(ctx, bound, into, from, n);
    leave_serial();
    return st;
}

static dftu_status userial_finalize(dftu_ctx* ctx, void* bound,
                                    void* const* states, uint64_t n,
                                    dftu_series** out) {
    dftu_status st;
    enter_serial();
    st = usum_finalize(ctx, bound, states, n, out);
    leave_serial();
    return st;
}

static void userial_destroy(void* bound, void* const* states, uint64_t n) {
    (void)bound;
    (void)states;
    (void)n;
    enter_serial();
    leave_serial();
}

static dftu_status userial_serialize(dftu_ctx* ctx, void* bound,
                                     const void* state, uint8_t* out,
                                     uint64_t cap, uint64_t* size) {
    (void)ctx;
    (void)bound;
    enter_serial();
    atomic_fetch_add(&g_serial_serializes, 1);
    *size = sizeof(Sum);
    if (out && cap >= sizeof(Sum)) memcpy(out, state, sizeof(Sum));
    leave_serial();
    return DFTU_OK;
}

static dftu_status userial_deserialize(dftu_ctx* ctx, void* bound,
                                       const uint8_t* in, uint64_t n,
                                       void* state) {
    dftu_status st = DFTU_OK;
    (void)bound;
    enter_serial();
    if (n != sizeof(Sum))
        st = dftu_fail(ctx, DFTU_E_INVALID, "userial: bad state size");
    else
        memcpy(state, in, sizeof(Sum));
    leave_serial();
    return st;
}

static void userial_free_bound(void* bound) {
    enter_serial();
    free_bound(bound);
    leave_serial();
}

/* udistinct */

typedef struct {
    int64_t* v;
    uint64_t n;
    uint64_t cap;
} Set;

static int set_add(Set* s, int64_t x) {
    uint64_t i;
    for (i = 0; i < s->n; ++i)
        if (s->v[i] == x) return 1;
    if (s->n == s->cap) {
        uint64_t cap = s->cap ? s->cap * 2 : 4;
        int64_t* grown = (int64_t*)realloc(s->v, cap * sizeof(int64_t));
        if (!grown) return 0;
        s->v = grown;
        s->cap = cap;
    }
    s->v[s->n++] = x;
    return 1;
}

static dftu_status udistinct_bind(dftu_ctx* ctx, void* self,
                                  const dftu_type_info* args, uint32_t n,
                                  const dftu_scalar* const* consts,
                                  dftu_type_info* out, void** bound) {
    (void)self;
    (void)consts;
    return bind_as(ctx, args, n, out, bound, DFTU_TYPE_INT64, 0, 1);
}

static dftu_status udistinct_update(dftu_ctx* ctx, void* bound,
                                    void* const* states,
                                    const dftu_series* const* args,
                                    uint32_t nargs, uint64_t rows) {
    uint64_t i;
    (void)bound;
    (void)nargs;
    for (i = 0; i < rows; ++i) {
        if (!states[i]) continue;
        if (!set_add((Set*)states[i],
                     ((const int64_t*)dftu_series_data(args[0]))[i]))
            return dftu_fail(ctx, DFTU_E_NO_MEMORY, "udistinct: out of memory");
    }
    return DFTU_OK;
}

static dftu_status udistinct_combine(dftu_ctx* ctx, void* bound,
                                     void* const* into, void* const* from,
                                     uint64_t n) {
    uint64_t i, j;
    (void)bound;
    for (i = 0; i < n; ++i) {
        const Set* f = (const Set*)from[i];
        for (j = 0; j < f->n; ++j)
            if (!set_add((Set*)into[i], f->v[j]))
                return dftu_fail(ctx, DFTU_E_NO_MEMORY,
                                 "udistinct: out of memory");
    }
    return DFTU_OK;
}

static dftu_status udistinct_finalize(dftu_ctx* ctx, void* bound,
                                      void* const* states, uint64_t n,
                                      dftu_series** out) {
    int64_t* v = (int64_t*)malloc(n * sizeof(int64_t) + 1);
    uint64_t i;
    (void)ctx;
    (void)bound;
    for (i = 0; i < n; ++i) v[i] = (int64_t)((const Set*)states[i])->n;
    *out = dftu_series_new_flat(DFTU_TYPE_INT64, v, (int64_t)n, NULL);
    free(v);
    return DFTU_OK;
}

static void udistinct_destroy(void* bound, void* const* states, uint64_t n) {
    uint64_t i;
    (void)bound;
    for (i = 0; i < n; ++i) {
        free(((Set*)states[i])->v);
        ++g_destroys;
    }
}

static dftu_status udistinct_serialize(dftu_ctx* ctx, void* bound,
                                       const void* state, uint8_t* out,
                                       uint64_t cap, uint64_t* size) {
    const Set* s = (const Set*)state;
    (void)ctx;
    (void)bound;
    *size = s->n * sizeof(int64_t);
    if (out && cap >= *size && s->n) memcpy(out, s->v, (size_t)*size);
    return DFTU_OK;
}

static dftu_status udistinct_deserialize(dftu_ctx* ctx, void* bound,
                                         const uint8_t* in, uint64_t n,
                                         void* state) {
    Set* s = (Set*)state;
    uint64_t i;
    (void)bound;
    if (n % sizeof(int64_t) != 0)
        return dftu_fail(ctx, DFTU_E_INVALID, "udistinct: bad state bytes");
    for (i = 0; i < n / sizeof(int64_t); ++i) {
        int64_t x;
        memcpy(&x, in + i * sizeof(int64_t), sizeof(x));
        if (!set_add(s, x))
            return dftu_fail(ctx, DFTU_E_NO_MEMORY, "udistinct: out of memory");
    }
    return DFTU_OK;
}

/* ucountall */

static dftu_status ucountall_bind(dftu_ctx* ctx, void* self,
                                  const dftu_type_info* args, uint32_t n,
                                  const dftu_scalar* const* consts,
                                  dftu_type_info* out, void** bound) {
    (void)self;
    (void)consts;
    return bind_as(ctx, args, n, out, bound, DFTU_TYPE_INT64, 0, 0);
}

static dftu_status ucountall_update(dftu_ctx* ctx, void* bound,
                                    void* const* states,
                                    const dftu_series* const* args,
                                    uint32_t nargs, uint64_t rows) {
    uint64_t i;
    (void)ctx;
    (void)bound;
    (void)args;
    (void)nargs;
    for (i = 0; i < rows; ++i)
        if (states[i]) ++*(int64_t*)states[i];
    return DFTU_OK;
}

static dftu_status ucountall_combine(dftu_ctx* ctx, void* bound,
                                     void* const* into, void* const* from,
                                     uint64_t n) {
    uint64_t i;
    (void)ctx;
    (void)bound;
    for (i = 0; i < n; ++i) *(int64_t*)into[i] += *(const int64_t*)from[i];
    return DFTU_OK;
}

static dftu_status ucountall_finalize(dftu_ctx* ctx, void* bound,
                                      void* const* states, uint64_t n,
                                      dftu_series** out) {
    int64_t* v = (int64_t*)malloc(n * sizeof(int64_t) + 1);
    uint64_t i;
    (void)ctx;
    (void)bound;
    for (i = 0; i < n; ++i) v[i] = *(const int64_t*)states[i];
    *out = dftu_series_new_flat(DFTU_TYPE_INT64, v, (int64_t)n, NULL);
    free(v);
    return DFTU_OK;
}

/* ufail, ubadrows */

static dftu_status ufail_update(dftu_ctx* ctx, void* bound, void* const* states,
                                const dftu_series* const* args, uint32_t nargs,
                                uint64_t rows) {
    (void)bound;
    (void)states;
    (void)args;
    (void)nargs;
    (void)rows;
    return dftu_fail(ctx, DFTU_E_INVALID, "ufail: refused on purpose");
}

static dftu_status ubadrows_finalize(dftu_ctx* ctx, void* bound,
                                     void* const* states, uint64_t n,
                                     dftu_series** out) {
    int64_t* v = (int64_t*)calloc(n + 1, sizeof(int64_t));
    (void)ctx;
    (void)bound;
    (void)states;
    *out = dftu_series_new_flat(DFTU_TYPE_INT64, v, (int64_t)n + 1, NULL);
    free(v);
    return DFTU_OK;
}

static void init_align(void* const* states, uint64_t n, uintptr_t align) {
    uint64_t i;
    for (i = 0; i < n; ++i)
        if ((uintptr_t)states[i] % align != 0) ++g_misaligned;
}

static void init_align32(void* bound, void* const* states, uint64_t n) {
    (void)bound;
    init_align(states, n, 32);
}

static void init_align16(void* bound, void* const* states, uint64_t n) {
    (void)bound;
    init_align(states, n, 16);
}

static dftu_status ufailcombine_combine(dftu_ctx* ctx, void* bound,
                                        void* const* into, void* const* from,
                                        uint64_t n) {
    (void)bound;
    (void)into;
    (void)from;
    (void)n;
    return dftu_fail(ctx, DFTU_E_INVALID, "ufailcombine: refused merge");
}

/* uwsum, ucount: a UDAF of two arguments and one of none */

static dftu_status uwsum_bind(dftu_ctx* ctx, void* self,
                              const dftu_type_info* args, uint32_t n,
                              const dftu_scalar* const* consts,
                              dftu_type_info* out, void** bound) {
    (void)self;
    (void)consts;
    if (n != 2 || args[1].id != DFTU_TYPE_INT64)
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "udaf_plugin: the argument type is not accepted");
    return bind_as(ctx, args, 1, out, bound, DFTU_TYPE_INT64, 0, 1);
}

static dftu_status uwsum_update(dftu_ctx* ctx, void* bound, void* const* states,
                                const dftu_series* const* args, uint32_t nargs,
                                uint64_t rows) {
    const int64_t* x = (const int64_t*)dftu_series_data(args[0]);
    const int64_t* w = (const int64_t*)dftu_series_data(args[1]);
    uint64_t i;
    (void)ctx;
    (void)bound;
    if (nargs != 2)
        return dftu_fail(ctx, DFTU_E_INVALID, "uwsum: expected two arguments");
    for (i = 0; i < rows; ++i)
        if (states[i]) ((Sum*)states[i])->i += x[i] * w[i];
    return DFTU_OK;
}

static dftu_status ucount_bind(dftu_ctx* ctx, void* self,
                               const dftu_type_info* args, uint32_t n,
                               const dftu_scalar* const* consts,
                               dftu_type_info* out, void** bound) {
    Bound* b;
    (void)self;
    (void)args;
    (void)consts;
    if (n != 0)
        return dftu_fail(ctx, DFTU_E_INVALID, "ucount takes no argument");
    b = (Bound*)calloc(1, sizeof(Bound));
    if (!b) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    b->out = DFTU_TYPE_INT64;
    ++g_binds;
    *bound = b;
    out->struct_size = sizeof(*out);
    out->id = DFTU_TYPE_INT64;
    return DFTU_OK;
}

#define POD_FIELDS(NAME, FLAGS, SIZE, ALIGN, BIND, UPDATE, COMBINE, FINALIZE) \
    {sizeof(dftu_udaf_desc),                                                  \
     NAME,                                                                    \
     FLAGS,                                                                   \
     1,                                                                       \
     1,                                                                       \
     SIZE,                                                                    \
     ALIGN,                                                                   \
     BIND,                                                                    \
     count_inits,                                                             \
     UPDATE,                                                                  \
     COMBINE,                                                                 \
     FINALIZE,                                                                \
     NULL,                                                                    \
     NULL,                                                                    \
     NULL,                                                                    \
     NULL,                                                                    \
     NULL,                                                                    \
     free_bound}

static const dftu_udaf_desc UDAFS[] = {
    POD_FIELDS("usum", DFTU_AGG_STATE_POD | DFTU_AGG_PARALLEL_SAFE, sizeof(Sum),
               8, usum_bind, usum_update, usum_combine, usum_finalize),
    {sizeof(dftu_udaf_desc), "udistinct", DFTU_AGG_PARALLEL_SAFE, 1, 1,
     sizeof(Set), 8, udistinct_bind, count_inits, udistinct_update,
     udistinct_combine, udistinct_finalize, udistinct_destroy,
     udistinct_serialize, udistinct_deserialize, NULL, NULL, free_bound},
    POD_FIELDS("ucountall", DFTU_AGG_STATE_POD | DFTU_AGG_NULL_AWARE,
               sizeof(int64_t), 8, ucountall_bind, ucountall_update,
               ucountall_combine, ucountall_finalize),
    POD_FIELDS("ufail", DFTU_AGG_STATE_POD, sizeof(int64_t), 8, ucountall_bind,
               ufail_update, ucountall_combine, ucountall_finalize),
    {sizeof(dftu_udaf_desc), "ualign32", DFTU_AGG_STATE_POD, 1, 1, 24, 32,
     ucountall_bind, init_align32, ucountall_update, ucountall_combine,
     ucountall_finalize, NULL, NULL, NULL, NULL, NULL, free_bound},
    {sizeof(dftu_udaf_desc), "ualign16", DFTU_AGG_STATE_POD, 1, 1, 40, 16,
     ucountall_bind, init_align16, ucountall_update, ucountall_combine,
     ucountall_finalize, NULL, NULL, NULL, NULL, NULL, free_bound},
    POD_FIELDS("ufailcombine", DFTU_AGG_STATE_POD, sizeof(int64_t), 8,
               ucountall_bind, ucountall_update, ufailcombine_combine,
               ucountall_finalize),
    {sizeof(dftu_udaf_desc), "userial", 0, 1, 1, sizeof(Sum), 8, userial_bind,
     userial_init, userial_update, userial_combine, userial_finalize,
     userial_destroy, userial_serialize, userial_deserialize, NULL, NULL,
     userial_free_bound},
    POD_FIELDS("ubadrows", DFTU_AGG_STATE_POD, sizeof(int64_t), 8,
               ucountall_bind, ucountall_update, ucountall_combine,
               ubadrows_finalize),
    {sizeof(dftu_udaf_desc), "uwsum",
     DFTU_AGG_STATE_POD | DFTU_AGG_PARALLEL_SAFE, 2, 2, sizeof(Sum), 8,
     uwsum_bind, count_inits, uwsum_update, usum_combine, usum_finalize, NULL,
     NULL, NULL, NULL, NULL, free_bound},
    {sizeof(dftu_udaf_desc), "usumr",
     DFTU_AGG_STATE_POD | DFTU_AGG_PARALLEL_SAFE | DFTU_AGG_HAS_RETRACT, 1, 1,
     sizeof(Sum), 8, usum_bind, count_inits, usum_update, usum_combine,
     usum_finalize, NULL, NULL, NULL, usumr_retract, usumr_value, free_bound},
    {sizeof(dftu_udaf_desc), "uretfail",
     DFTU_AGG_STATE_POD | DFTU_AGG_HAS_RETRACT, 1, 1, sizeof(Sum), 8, usum_bind,
     count_inits, usum_update, usum_combine, usum_finalize, NULL, NULL, NULL,
     uretfail_retract, usumr_value, free_bound},
    {sizeof(dftu_udaf_desc), "uwneg", DFTU_AGG_STATE_POD | DFTU_AGG_HAS_RETRACT,
     1, 1, sizeof(Sum), 8, uwneg_bind, count_inits, usum_update, usum_combine,
     usum_finalize, NULL, NULL, NULL, usumr_retract, uwneg_value, free_bound},
    {sizeof(dftu_udaf_desc), "uwbig", DFTU_AGG_STATE_POD | DFTU_AGG_HAS_RETRACT,
     1, 1, sizeof(Sum), 8, uwbig_bind, count_inits, usum_update, usum_combine,
     usum_finalize, NULL, NULL, NULL, usumr_retract, uwbig_value, free_bound},
    {sizeof(dftu_udaf_desc), "ucount",
     DFTU_AGG_STATE_POD | DFTU_AGG_PARALLEL_SAFE, 0, 0, sizeof(int64_t), 8,
     ucount_bind, count_inits, ucountall_update, ucountall_combine,
     ucountall_finalize, NULL, NULL, NULL, NULL, NULL, free_bound},
};

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("udaf_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_ops* ops;
    size_t i;
    (void)config;
    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (!ops || !ops->register_udaf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udaf registry at load");
    for (i = 0; i < sizeof(UDAFS) / sizeof(UDAFS[0]); ++i) {
        const dftu_status st = ops->register_udaf(ctx, h->h, &UDAFS[i], NULL);
        if (st < 0) return st;
    }
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    *out = &g_plugin;
    return DFTU_OK;
}
