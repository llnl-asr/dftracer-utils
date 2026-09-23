/* Example dftracer-utils plugin in pure C: a weighted process<->file bipartite
 * graph, and the plain shape of a dft.ext.agg accumulator - a two-column key
 * plus several aggregates over the same batch. Each batch arrives as a column
 * dataframe and is folded into one accumulator grouping by {pid, fhash} with
 * the edge count and the total and mean duration on the edge. The host merges
 * it across workers and finalizes it to a dataframe [pid, fhash, edges,
 * dur_sum, dur_mean], returned from run() under "process_file_edges", from
 * which the caller builds an adjacency matrix in bulk with no per-edge Python.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o process_file_edges.so process_file_edges.c
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>

/* The accumulator lives host-side; the slice only marks make_slice non-null so
 * the fold delivers batches. */
static void* make_slice(void* self) {
    (void)self;
    return calloc(1, 1);
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    const dftu_svc_agg* agg =
        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
    static const char* const keys[2] = {"pid", "fhash"};
    static const dftu_agg_col specs[3] = {
        {DFTU_AGG_COUNT, NULL, "edges", 0.0, NULL},
        {DFTU_AGG_SUM, "dur", "dur_sum", 0.0, NULL},
        {DFTU_AGG_MEAN, "dur", "dur_mean", 0.0, NULL}};
    dftu_agg* a;
    (void)slice;
    if (!agg || !agg->agg_new) return NULL;
    a = agg->agg_new(host->h, "process_file_edges", keys, 2, specs, 3);
    /* A batch whose events carry no fhash column has agg_accumulate skip it
     * rather than fold a partial key. */
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
