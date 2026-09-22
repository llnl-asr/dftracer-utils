/* Example dftracer-utils plugin in pure C, implementing the vtable directly
 * against abi.h with no C++ wrapper: counts events and tracks max duration.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o event_counter.so event_counter.c
 * Run:   dftracer_run -d ./traces --plugin ./event_counter.so
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint64_t events;
    uint64_t with_dur;
    uint64_t max_dur;
} Counter;

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, sizeof(Counter));
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    Counter* c = (Counter*)slice;
    int64_t n = dftu_dataframe_num_rows(df);
    dftu_series* dur_col = dftu_dataframe_column(df, "dur");
    dftu_series* ph_col = dftu_dataframe_column(df, "ph");
    const uint64_t* dur = NULL;
    const int64_t* ph = NULL;
    int64_t i;
    (void)host;
    if (dur_col && dftu_series_type(dur_col) == DFTU_TYPE_UINT64)
        dur = (const uint64_t*)dftu_series_data(dur_col);
    if (ph_col && dftu_series_type(ph_col) == DFTU_TYPE_INT64)
        ph = (const int64_t*)dftu_series_data(ph_col);
    for (i = 0; i < n; ++i) {
        c->events++;
        /* Mirrors plugins::Event::has_dur(): the row-fold engine keeps no
         * per-row null for `dur`, so "has a duration" is phase() == Complete,
         * read off the `ph` column (a DFTU_PH_* code). */
        if (dur && ph && ph[i] == DFTU_PH_COMPLETE) {
            c->with_dur++;
            if (dur[i] > c->max_dur) c->max_dur = dur[i];
        }
    }
    if (dur_col) dftu_series_free(dur_col);
    if (ph_col) dftu_series_free(ph_col);
    return NULL; /* synchronous */
}

static void merge(void* into, void* other) {
    Counter* a = (Counter*)into;
    const Counter* o = (const Counter*)other;
    a->events += o->events;
    a->with_dur += o->with_dur;
    if (o->max_dur > a->max_dur) a->max_dur = o->max_dur;
}

static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
    const Counter* c = (const Counter*)slice;
    char line[128];
    int n =
        snprintf(line, sizeof line, "events=%llu with_dur=%llu max_dur=%llu us",
                 (unsigned long long)c->events, (unsigned long long)c->with_dur,
                 (unsigned long long)c->max_dur);
    if (n > 0) host->log(host->h, DFTU_LOG_INFO, line, (uint32_t)n);
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
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    return &g_plugin;
}
