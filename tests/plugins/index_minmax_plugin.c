// SPDX-License-Identifier: MIT
/* Test-only plugin registering the index extension "index_minmax.range": per
 * chunk and per file, the min and max of the top-level number "v" (the key is
 * found by text, which the test data allows). Comparisons on "v" prune
 * through it. The config key "version" sets the extension version.
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double min;
    double max;
    uint64_t n;
} range;

typedef struct {
    range chunk;
    range file;
} builder;

typedef struct {
    int32_t op;
    double value;
} compiled;

static void add(range* r, double v) {
    if (r->n == 0 || v < r->min) r->min = v;
    if (r->n == 0 || v > r->max) r->max = v;
    ++r->n;
}

/* Set from the config "fail_step": step then fails, to show the host reports
   a failed step with the plugin's message. */
static int g_fail_step;

static dftu_status make_builder(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(builder));
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    return DFTU_OK;
}

static dftu_status step(dftu_ctx* ctx, void* b, const dftu_bytes* lines,
                        uint64_t n) {
    builder* bl = (builder*)b;
    if (g_fail_step)
        return dftu_fail(ctx, DFTU_E_PARSE, "index_minmax: refused a chunk");
    static const char KEY[] = "\"v\":";
    for (uint64_t i = 0; i < n; ++i) {
        const char* p = (const char*)lines[i].data;
        const char* end = p + lines[i].len;
        for (; p + sizeof(KEY) - 1 <= end; ++p) {
            if (memcmp(p, KEY, sizeof(KEY) - 1) != 0) continue;
            char* num_end;
            double v = strtod(p + sizeof(KEY) - 1, &num_end);
            if (num_end != p + sizeof(KEY) - 1) {
                add(&bl->chunk, v);
                add(&bl->file, v);
            }
            break;
        }
    }
    return DFTU_OK;
}

static dftu_status finish_granule(dftu_ctx* ctx, void* b, uint64_t granule,
                                  const dftu_index_out* out) {
    builder* bl = (builder*)b;
    (void)ctx;
    (void)granule;
    out->append(out->self, &bl->chunk, sizeof(range));
    memset(&bl->chunk, 0, sizeof(range));
    return DFTU_OK;
}

static dftu_status finish_file(dftu_ctx* ctx, void* b,
                               const dftu_index_out* out) {
    (void)ctx;
    out->append(out->self, &((builder*)b)->file, sizeof(range));
    return DFTU_OK;
}

static void destroy_builder(void* b) { free(b); }

static dftu_status compile(dftu_ctx* ctx, void* self,
                           const dftu_index_leaf* leaf, void** out) {
    compiled* c;
    (void)self;
    *out = NULL;
    if (leaf->any || strcmp(leaf->path, "v") != 0) return DFTU_OK;
    if (leaf->op != DFTU_INDEX_EQ && leaf->op != DFTU_INDEX_LT &&
        leaf->op != DFTU_INDEX_LE && leaf->op != DFTU_INDEX_GT &&
        leaf->op != DFTU_INDEX_GE)
        return DFTU_OK;
    if (leaf->value->kind != DFTU_VAL_I64 && leaf->value->kind != DFTU_VAL_F64)
        return DFTU_OK;
    c = (compiled*)malloc(sizeof(compiled));
    if (!c) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    c->op = leaf->op;
    c->value = dftu_as_f64(leaf->value, 0);
    *out = c;
    return DFTU_OK;
}

static int judge(void* h, const uint8_t* p, uint64_t n) {
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

static void release(void* h) { free(h); }

static dftu_index_extension g_ext;

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

DFTU_PLUGIN_INFO("index_minmax", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_index* index =
        (const dftu_svc_index*)h->get_service(h->h, DFTU_SVC_INDEX);
    dftu_status st;
    if (!index)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no index service at load");
    memset(&g_ext, 0, sizeof(g_ext));
    g_ext.struct_size = sizeof(g_ext);
    g_ext.version = (uint32_t)dftu_as_i64(dftu_obj_get(config, "version"), 1);
    g_fail_step = dftu_as_bool(dftu_obj_get(config, "fail_step"), 0);
    g_ext.make_builder = make_builder;
    g_ext.step = step;
    g_ext.finish_granule = finish_granule;
    g_ext.finish_file = finish_file;
    g_ext.destroy_builder = destroy_builder;
    g_ext.compile = compile;
    g_ext.may_match = judge;
    g_ext.file_may_match = judge;
    g_ext.release = release;
    st = index->register_extension(ctx, h->h, "index_minmax.range", &g_ext,
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
