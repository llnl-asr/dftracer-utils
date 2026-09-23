/* A real C consumer of the dftu_svc_ops ABI: call a host utility that is now a
 * named op (dftu.hash.fnv1a) on a column the consumer builds itself - the same
 * path a C plugin uses to reach the host's call-host registry. */
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

/* Hash `data` with the host's dftu.hash.fnv1a op; 0 ok, -1 on failure. */
int dftu_test_ops_fnv1a(const dftu_plugin_host* h, const char* data,
                        uint32_t len, uint64_t* out) {
    const dftu_svc_ops* ops =
        (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    int32_t offsets[2];
    dftu_series* in;
    dftu_series* hashed;
    dftu_result_series res;
    const uint64_t* vals;
    if (!ops || !ops->run) return -1;
    offsets[0] = 0;
    offsets[1] = (int32_t)len;
    in = dftu_series_new_string(DFTU_TYPE_STRING, offsets, data, 1, NULL);
    if (!in) return -1;
    res = ops->run(h->h, "dftu.hash.fnv1a", (const dftu_series* const*)&in, 1,
                   NULL);
    dftu_series_free(in);
    if (!DFTU_RESULT_OK(res)) return -1;
    hashed = DFTU_RESULT_VALUE(res);
    vals = (const uint64_t*)dftu_series_data(hashed);
    if (vals) *out = vals[0];
    dftu_series_free(hashed);
    return vals ? 0 : -1;
}

/* The registry is reachable by name, and find exposes the op's signature. */
int dftu_test_ops_find(const dftu_plugin_host* h) {
    const dftu_svc_ops* ops =
        (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    const dftu_op_desc* desc;
    if (!ops || !ops->find) return -1;
    desc = ops->find(h->h, "dftu.hex.parse64");
    return (desc && desc->fn) ? 1 : 0;
}
