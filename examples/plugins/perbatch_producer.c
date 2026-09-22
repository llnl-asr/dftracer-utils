/* Example producer plugin: each batch it computes a derived value (the count of
 * events carrying a duration) and publishes it on the batch-scoped port named
 * com.example.perbatch_dur, so a consumer running later in the same fuse order
 * reads it for the same batch.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o perbatch_producer.so perbatch_producer.c
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdlib.h>
#include <string.h>

#define PERBATCH_PORT "com.example.perbatch_dur"

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, 1);
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    (void)slice;
    const dftu_svc_ports* p =
        (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);
    if (p) {
        int64_t n = dftu_dataframe_num_rows(df);
        dftu_series* ph_col = dftu_dataframe_column(df, "ph");
        const int64_t* ph = NULL;
        uint64_t with_dur = 0;
        int64_t i;
        if (ph_col && dftu_series_type(ph_col) == DFTU_TYPE_INT64)
            ph = (const int64_t*)dftu_series_data(ph_col);
        /* Mirrors plugins::Event::has_dur(): "has a duration" is phase() ==
         * Complete, read off the `ph` column (a DFTU_PH_* code). */
        if (ph)
            for (i = 0; i < n; ++i)
                if (ph[i] == DFTU_PH_COMPLETE) ++with_dur;
        if (ph_col) dftu_series_free(ph_col);
        p->publish(host->h, p->port_key(host->h, PERBATCH_PORT), &with_dur,
                   (uint32_t)sizeof with_dur);
    }
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

static const char* const provided[2] = {PERBATCH_PORT, NULL};

static const char* const* provides(void* self) {
    (void)self;
    return provided;
}

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
    g_plugin.provides = provides;
    return &g_plugin;
}
