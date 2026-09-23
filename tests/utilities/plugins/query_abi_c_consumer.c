/* A real C consumer of the dftu_svc_query ABI: compiled by the C compiler,
 * sees only abi.h. The C++ harness builds the host, interns the strings, and
 * constructs the events; this compiles the filter and matches through the raw
 * vtable. */
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

/* Compile `src` and match row `row` of `df`: 1 match, 0 no-match, -1 on compile
 * failure. */
int dftu_test_query_match(const dftu_plugin_host* h, const char* src,
                          uint32_t len, const dftu_dataframe* df, int64_t row) {
    const dftu_svc_query* q =
        (const dftu_svc_query*)h->get_service(h->h, DFTU_SVC_QUERY);
    if (!q) return -1;
    dftu_query* compiled = q->query_compile(h->h, src, len);
    if (!compiled) return -1;
    return q->query_matches(h->h, compiled, df, row);
}

/* A null query must match to a defined 0 across the ABI, never trap. */
int dftu_test_query_null_is_safe(const dftu_plugin_host* h,
                                 const dftu_dataframe* df, int64_t row) {
    const dftu_svc_query* q =
        (const dftu_svc_query*)h->get_service(h->h, DFTU_SVC_QUERY);
    if (!q) return -1;
    return q->query_matches(h->h, NULL, df, row);
}
