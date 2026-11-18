// SPDX-License-Identifier: MIT
/* Test-only plugin registering two index extensions that read typed columns
 * through step_column and no record line:
 *   "index_columns.range": the min and max of the int column "args.v" per
 *     chunk and per file (the extension that index_minmax.range is for lines);
 *   "index_columns.bloom": a bit set of the host's canonical hashes of the
 *     string column "args.s", from hash_series, tagged with the hash version.
 *   "index_columns.never": a flag, per chunk, that the column "dur.sketch" was
 *     passed (a path a record schema marks never_index);
 *   "index_columns.star": takes every path, and keeps a flag per chunk for the
 *     columns "ts" and "dur.sketch".
 * A leaf prunes only when the host says the column has the type each one
 * stored (leaf->type), so the tests also show that the type is filled.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t min;
    int64_t max;
    uint64_t n;
} range;

enum { BITS = 4096, BLOOM_BYTES = 1 + BITS / 8 };

typedef struct {
    range chunk;
    range file;
    uint8_t bloom[BLOOM_BYTES];
} builder;

typedef struct {
    int32_t op;
    int64_t value;
    uint64_t hash;
} compiled;

static const dftu_svc_index* g_index;
static const char* const RANGE_COLUMNS[] = {"args.v", NULL};
static const char* const BLOOM_COLUMNS[] = {"args.s", NULL};
static const char* const NEVER_COLUMNS[] = {"dur.sketch", NULL};
static const char* const STAR_COLUMNS[] = {"*", NULL};
static dftu_index_extension g_never;
static dftu_index_extension g_star;
static dftu_index_extension g_range;
static dftu_index_extension g_bloom;

static void add(range* r, int64_t v) {
    if (r->n == 0 || v < r->min) r->min = v;
    if (r->n == 0 || v > r->max) r->max = v;
    ++r->n;
}

static dftu_status make_builder(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(builder));
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    ((builder*)*out)->bloom[0] = (uint8_t)g_index->hash_version;
    return DFTU_OK;
}

static dftu_status range_column(dftu_ctx* ctx, void* b, const char* name,
                                uint32_t name_len, const dftu_series* col) {
    builder* bl = (builder*)b;
    const int64_t* data;
    int64_t n = dftu_series_length(col);
    (void)name;
    (void)name_len;
    if (dftu_series_type(col) != DFTU_TYPE_INT64)
        return dftu_fail(ctx, DFTU_E_INVALID, "index_columns: not an int64");
    data = (const int64_t*)dftu_series_data(col);
    for (int64_t i = 0; i < n; ++i) {
        if (dftu_series_is_null(col, i)) continue;
        add(&bl->chunk, data[i]);
        add(&bl->file, data[i]);
    }
    return DFTU_OK;
}

static dftu_status bloom_column(dftu_ctx* ctx, void* b, const char* name,
                                uint32_t name_len, const dftu_series* col) {
    builder* bl = (builder*)b;
    uint64_t n = (uint64_t)dftu_series_length(col);
    uint64_t* hashes = (uint64_t*)malloc(n * sizeof(uint64_t) + 1);
    (void)name;
    (void)name_len;
    if (!hashes) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    if (g_index->hash_series(col, hashes, n) < 0) {
        free(hashes);
        return dftu_fail(ctx, DFTU_E_INVALID, "index_columns: hash_series");
    }
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t bit = hashes[i] % BITS;
        if (dftu_series_is_null(col, (int64_t)i)) continue;
        bl->bloom[1 + bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
    free(hashes);
    return DFTU_OK;
}

static dftu_status finish_range(dftu_ctx* ctx, void* b, uint64_t granule,
                                const dftu_index_out* out) {
    builder* bl = (builder*)b;
    (void)ctx;
    (void)granule;
    out->append(out->self, &bl->chunk, sizeof(range));
    memset(&bl->chunk, 0, sizeof(range));
    return DFTU_OK;
}

static dftu_status finish_range_file(dftu_ctx* ctx, void* b,
                                     const dftu_index_out* out) {
    (void)ctx;
    out->append(out->self, &((builder*)b)->file, sizeof(range));
    return DFTU_OK;
}

static dftu_status finish_bloom(dftu_ctx* ctx, void* b, uint64_t granule,
                                const dftu_index_out* out) {
    builder* bl = (builder*)b;
    (void)ctx;
    (void)granule;
    out->append(out->self, bl->bloom, BLOOM_BYTES);
    memset(bl->bloom + 1, 0, BLOOM_BYTES - 1);
    return DFTU_OK;
}

static void destroy_builder(void* b) { free(b); }

/* Flags builders: bloom[1] is "ts" seen, bloom[2] is "dur.sketch" seen. */
static dftu_status flag_column(dftu_ctx* ctx, void* b, const char* name,
                               uint32_t name_len, const dftu_series* col) {
    builder* bl = (builder*)b;
    (void)ctx;
    (void)name_len;
    (void)col;
    if (strcmp(name, "ts") == 0) bl->bloom[1] = 1;
    if (strcmp(name, "dur.sketch") == 0) bl->bloom[2] = 1;
    return DFTU_OK;
}

static dftu_status finish_flags(dftu_ctx* ctx, void* b, uint64_t granule,
                                const dftu_index_out* out) {
    builder* bl = (builder*)b;
    (void)ctx;
    (void)granule;
    out->append(out->self, bl->bloom + 1, 2);
    bl->bloom[1] = 0;
    bl->bloom[2] = 0;
    return DFTU_OK;
}

static dftu_status compile_flag(dftu_ctx* ctx, void* self,
                                const dftu_index_leaf* leaf, void** out) {
    compiled* c;
    int is_ts = strcmp(leaf->path, "ts") == 0;
    (void)self;
    *out = NULL;
    if (!is_ts && strcmp(leaf->path, "dur.sketch") != 0) return DFTU_OK;
    c = (compiled*)malloc(sizeof(compiled));
    if (!c) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    c->op = is_ts ? 0 : 1;
    *out = c;
    return DFTU_OK;
}

/* Op 0 and 1 keep a chunk that was passed "ts" or "dur.sketch"; op 2 keeps a
   chunk that was not passed "dur.sketch", so that extension prunes a chunk
   only on evidence it should not have. */
static int judge_flag(void* h, const uint8_t* p, uint64_t n) {
    const compiled* c = (const compiled*)h;
    if (n != 2) return 1;
    if (c->op == 2) return p[1] == 0;
    return p[c->op] != 0;
}

static dftu_status compile_never(dftu_ctx* ctx, void* self,
                                 const dftu_index_leaf* leaf, void** out) {
    dftu_status st = compile_flag(ctx, self, leaf, out);
    if (st >= 0 && *out && strcmp(leaf->path, "ts") == 0)
        ((compiled*)*out)->op = 2;
    return st;
}

static dftu_status compile_range(dftu_ctx* ctx, void* self,
                                 const dftu_index_leaf* leaf, void** out) {
    compiled* c;
    (void)self;
    *out = NULL;
    if (leaf->any || strcmp(leaf->path, "args.v") != 0) return DFTU_OK;
    if (leaf->type != DFTU_TYPE_INT64) return DFTU_OK;
    if (leaf->op != DFTU_INDEX_EQ && leaf->op != DFTU_INDEX_LT &&
        leaf->op != DFTU_INDEX_LE && leaf->op != DFTU_INDEX_GT &&
        leaf->op != DFTU_INDEX_GE)
        return DFTU_OK;
    if (leaf->value->kind != DFTU_VAL_I64) return DFTU_OK;
    c = (compiled*)malloc(sizeof(compiled));
    if (!c) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    c->op = leaf->op;
    c->value = leaf->value->as.i64;
    *out = c;
    return DFTU_OK;
}

static dftu_status compile_bloom(dftu_ctx* ctx, void* self,
                                 const dftu_index_leaf* leaf, void** out) {
    compiled* c;
    (void)self;
    *out = NULL;
    if (leaf->any || strcmp(leaf->path, "args.s") != 0) return DFTU_OK;
    if (leaf->type != DFTU_TYPE_STRING) return DFTU_OK;
    if (leaf->op != DFTU_INDEX_EQ || leaf->value->kind != DFTU_VAL_STR)
        return DFTU_OK;
    c = (compiled*)malloc(sizeof(compiled));
    if (!c) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    c->hash = g_index->hash_value(leaf->value);
    *out = c;
    return DFTU_OK;
}

static int judge_range(void* h, const uint8_t* p, uint64_t n) {
    const compiled* c = (const compiled*)h;
    range r;
    if (n != sizeof(range)) return 1;
    memcpy(&r, p, sizeof(range));
    if (r.n == 0) return 0;
    switch (c->op) {
        case DFTU_INDEX_EQ:
            return r.min <= c->value && c->value <= r.max;
        case DFTU_INDEX_LT:
            return r.min < c->value;
        case DFTU_INDEX_LE:
            return r.min <= c->value;
        case DFTU_INDEX_GT:
            return r.max > c->value;
        case DFTU_INDEX_GE:
            return r.max >= c->value;
        default:
            return 1;
    }
}

static int judge_bloom(void* h, const uint8_t* p, uint64_t n) {
    const compiled* c = (const compiled*)h;
    uint64_t bit = c->hash % BITS;
    if (n != BLOOM_BYTES || p[0] != g_index->hash_version) return 1;
    return (p[1 + bit / 8] >> (bit % 8)) & 1;
}

static void release(void* h) { free(h); }

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, 1);
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    return DFTU_OK;
}
static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    (void)ctx;
    (void)slice;
    (void)df;
    (void)host;
    return DFTU_OK;
}
static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
    (void)ctx;
    (void)into;
    (void)other;
    return DFTU_OK;
}
static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending) {
    (void)ctx;
    (void)slice;
    (void)host;
    (void)pending;
    return DFTU_OK;
}
static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("index_columns", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    dftu_status st;
    (void)config;
    g_index = (const dftu_svc_index*)h->get_service(h->h, DFTU_SVC_INDEX);
    if (!g_index)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no index service at load");

    memset(&g_never, 0, sizeof(g_never));
    g_never.struct_size = sizeof(g_never);
    g_never.version = 1;
    g_never.make_builder = make_builder;
    g_never.finish_granule = finish_flags;
    g_never.destroy_builder = destroy_builder;
    g_never.compile = compile_never;
    g_never.may_match = judge_flag;
    g_never.release = release;
    g_never.columns = NEVER_COLUMNS;
    g_never.step_column = flag_column;
    st = g_index->register_extension(ctx, h->h, "index_columns.never", &g_never,
                                     NULL);
    if (st < 0) return st;

    g_star = g_never;
    g_star.compile = compile_flag;
    g_star.columns = STAR_COLUMNS;
    st = g_index->register_extension(ctx, h->h, "index_columns.star", &g_star,
                                     NULL);
    if (st < 0) return st;

    memset(&g_range, 0, sizeof(g_range));
    g_range.struct_size = sizeof(g_range);
    g_range.version = 1;
    g_range.make_builder = make_builder;
    g_range.finish_granule = finish_range;
    g_range.finish_file = finish_range_file;
    g_range.destroy_builder = destroy_builder;
    g_range.compile = compile_range;
    g_range.may_match = judge_range;
    g_range.file_may_match = judge_range;
    g_range.release = release;
    g_range.columns = RANGE_COLUMNS;
    g_range.step_column = range_column;
    st = g_index->register_extension(ctx, h->h, "index_columns.range", &g_range,
                                     NULL);
    if (st < 0) return st;

    memset(&g_bloom, 0, sizeof(g_bloom));
    g_bloom.struct_size = sizeof(g_bloom);
    g_bloom.version = 1;
    g_bloom.make_builder = make_builder;
    g_bloom.finish_granule = finish_bloom;
    g_bloom.destroy_builder = destroy_builder;
    g_bloom.compile = compile_bloom;
    g_bloom.may_match = judge_bloom;
    g_bloom.release = release;
    g_bloom.columns = BLOOM_COLUMNS;
    g_bloom.step_column = bloom_column;
    st = g_index->register_extension(ctx, h->h, "index_columns.bloom", &g_bloom,
                                     NULL);
    if (st < 0) return st;

    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
