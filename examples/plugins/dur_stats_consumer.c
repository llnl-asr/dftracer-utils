/* Example consumer plugin: at finalize it asks dft.ext.agg for the producer's
 * cross-worker-merged accumulator by name; the host hands back the finalized
 * result as a one-row dataframe, which this plugin reads with the dataframe C
 * ABI and logs (WIRED). An empty scan creates no accumulator, so it logs the
 * no-data fallback instead.
 *
 * It names the accumulator in `consumes`, so the host finalizes the producer
 * first whatever order the two are given in, and refuses to run at all when no
 * loaded plugin provides that name.
 *
 * Build: cc -std=c11 -shared -fPIC -I<repo>/include \
 *           -o dur_stats_consumer.so dur_stats_consumer.c \
 *           -ldftracer_utils_dataframe
 */

#include <dftracer/utils/plugins/abi.h>
/* Reading a result frame needs the engine's column ops, so this example links
 * the dataframe C ABI; the other examples link nothing. */
#include <dftracer/utils/dataframe/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUR_STATS_PORT "com.example.dur_stats"

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, 1);
}

static void merge(void* into, void* other) {
    (void)into;
    (void)other;
}

/* Row 0 of `name` as a double; *ok stays untouched on success and is cleared
 * when the column is absent or of an unhandled type. */
static double cell(const dftu_dataframe* df, const char* name, int* ok) {
    dftu_series* col = dftu_dataframe_column(df, name);
    dftu_series* flat;
    const void* data;
    double out = 0.0;
    if (!col) {
        *ok = 0;
        return 0.0;
    }
    flat = dftu_series_materialize(col);
    dftu_series_free(col);
    if (!flat) {
        *ok = 0;
        return 0.0;
    }
    data = dftu_series_data(flat);
    if (!data || dftu_series_length(flat) < 1) {
        *ok = 0;
    } else {
        switch (dftu_series_type(flat)) {
            case DFTU_TYPE_INT64:
                out = (double)((const int64_t*)data)[0];
                break;
            case DFTU_TYPE_UINT64:
                out = (double)((const uint64_t*)data)[0];
                break;
            case DFTU_TYPE_FLOAT64:
                out = ((const double*)data)[0];
                break;
            default:
                *ok = 0;
                break;
        }
    }
    dftu_series_free(flat);
    return out;
}

static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
    (void)slice;
    const dftu_svc_agg* agg =
        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
    dftu_dataframe* res = NULL;
    char line[192];
    int n = 0;
    if (agg && agg->agg_result) res = agg->agg_result(host->h, DUR_STATS_PORT);
    if (res) {
        int ok = 1;
        double count = cell(res, "count", &ok);
        double p50 = cell(res, "p50", &ok);
        double lo = cell(res, "min", &ok);
        double hi = cell(res, "max", &ok);
        if (ok)
            n = snprintf(line, sizeof line,
                         "dur_stats_consumer: WIRED count=%llu p50=%.3f "
                         "min=%.3f max=%.3f",
                         (unsigned long long)count, p50, lo, hi);
        dftu_dataframe_free(res);
    }
    if (n == 0)
        n = snprintf(line, sizeof line,
                     "dur_stats_consumer: STANDALONE no producer");
    if (n > 0) host->log(host->h, DFTU_LOG_INFO, line, (uint32_t)n);
    return NULL;
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static const char* const consumed[2] = {DUR_STATS_PORT, NULL};

static const char* const* consumes(void* self) {
    (void)self;
    return consumed;
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
    (void)h;
    (void)config;
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.self = NULL;
    g_plugin.plan_query = NULL;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = NULL;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.consumes = consumes;
    return &g_plugin;
}
