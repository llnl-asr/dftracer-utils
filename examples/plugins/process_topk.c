/* Example dftracer-utils plugin in pure C: the bounded-size aggregates, whose
 * `param` carries k. Keyed by {pid}: TOPK and BOTTOMK keep the 3 event names
 * with the largest and smallest dur (ordered by the `by` column), APPROX_TOPK
 * the 5 most frequent names from a SpaceSaving sketch, and SAMPLE a
 * deterministic 5-name bottom-k-by-hash sample. Each stays bounded per group,
 * so the accumulator merges across workers in constant space. The finalized
 * dataframe is [pid, slowest3, fastest3, hottest5, sample5], returned from
 * run() under "process_topk"; hottest5 is a list<struct{value, count}> and the
 * rest are list<string>.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o process_topk.so process_topk.c
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, 1);
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    const dftu_svc_agg* agg =
        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
    static const char* const keys[1] = {"pid"};
    static const dftu_agg_col specs[4] = {
        {DFTU_AGG_TOPK, "name", "slowest3", 3.0, "dur"},
        {DFTU_AGG_BOTTOMK, "name", "fastest3", 3.0, "dur"},
        {DFTU_AGG_APPROX_TOPK, "name", "hottest5", 5.0, NULL},
        {DFTU_AGG_SAMPLE, "name", "sample5", 5.0, NULL}};
    dftu_agg* a;
    (void)slice;
    if (!agg || !agg->agg_new) return NULL;
    a = agg->agg_new(host->h, "process_topk", keys, 1, specs, 4);
    if (a) agg->agg_accumulate(host->h, a, df);
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

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
    (void)h;
    (void)config;
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.self = NULL;
    g_plugin.plan_query = NULL;
    g_plugin.make_slice = make_slice;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.on_batch = on_batch;
    return &g_plugin;
}
