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

static void* make_builder(void* self) {
    (void)self;
    return calloc(1, sizeof(builder));
}

static int step(void* b, const dftu_bytes* lines, uint64_t n) {
    builder* bl = (builder*)b;
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
    return 0;
}

static int finish_granule(void* b, uint64_t granule,
                          const dftu_index_out* out) {
    builder* bl = (builder*)b;
    (void)granule;
    out->append(out->ctx, &bl->chunk, sizeof(range));
    memset(&bl->chunk, 0, sizeof(range));
    return 0;
}

static int finish_file(void* b, const dftu_index_out* out) {
    out->append(out->ctx, &((builder*)b)->file, sizeof(range));
    return 0;
}

static void destroy_builder(void* b) { free(b); }

static void* compile(void* self, const dftu_index_leaf* leaf) {
    compiled* c;
    (void)self;
    if (leaf->any || strcmp(leaf->path, "v") != 0) return NULL;
    if (leaf->op != DFTU_INDEX_EQ && leaf->op != DFTU_INDEX_LT &&
        leaf->op != DFTU_INDEX_LE && leaf->op != DFTU_INDEX_GT &&
        leaf->op != DFTU_INDEX_GE)
        return NULL;
    if (leaf->value->kind != DFTU_VAL_I64 && leaf->value->kind != DFTU_VAL_F64)
        return NULL;
    c = (compiled*)malloc(sizeof(compiled));
    if (!c) return NULL;
    c->op = leaf->op;
    c->value = dftu_as_f64(leaf->value, 0);
    return c;
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

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, 1);
}
static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    (void)slice;
    (void)df;
    (void)host;
    return NULL;
}
static void merge(void* into, void* other) {
    (void)into;
    (void)other;
}
static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
    (void)slice;
    (void)host;
    return NULL;
}
static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                const dftu_value* config) {
    const dftu_svc_index* index =
        (const dftu_svc_index*)h->get_service(h->h, DFTU_SVC_INDEX);
    if (!index) return NULL;
    memset(&g_ext, 0, sizeof(g_ext));
    g_ext.version = (uint32_t)dftu_as_i64(dftu_obj_get(config, "version"), 1);
    g_ext.make_builder = make_builder;
    g_ext.step = step;
    g_ext.finish_granule = finish_granule;
    g_ext.finish_file = finish_file;
    g_ext.destroy_builder = destroy_builder;
    g_ext.compile = compile;
    g_ext.may_match = judge;
    g_ext.file_may_match = judge;
    g_ext.release = release;
    if (index->register_extension(h->h, "index_minmax.range", &g_ext, NULL) !=
        0)
        return NULL;

    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    return &g_plugin;
}
