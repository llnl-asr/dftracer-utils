/* A real C consumer of the dftu_svc_sketch ABI: create a quantile sketch, add
 * samples, read quantiles, and merge - all through the raw vtable from C. */
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

/* Add 1..100 (weight 1) and report the median and count. */
int dftu_test_sketch(const dftu_plugin_host* h, double* out_p50,
                     uint64_t* out_count) {
    const dftu_svc_sketch* s =
        (const dftu_svc_sketch*)h->get_service(h->h, DFTU_SVC_SKETCH);
    if (!s) return -1;
    dftu_sketch* sk = s->sketch_create(h->h);
    if (!sk) return -1;
    for (int i = 1; i <= 100; ++i) s->sketch_add(h->h, sk, (double)i, 1.0);
    dftu_quantiles q = s->sketch_result(h->h, sk);
    *out_p50 = q.p50;
    *out_count = q.count;
    s->sketch_free(h->h, sk);
    return 0;
}

/* Merge a 1..50 sketch with a 51..100 sketch; report the merged count. */
int dftu_test_sketch_merge(const dftu_plugin_host* h, uint64_t* out_count) {
    const dftu_svc_sketch* s =
        (const dftu_svc_sketch*)h->get_service(h->h, DFTU_SVC_SKETCH);
    if (!s) return -1;
    dftu_sketch* a = s->sketch_create(h->h);
    dftu_sketch* b = s->sketch_create(h->h);
    if (!a || !b) return -1;
    for (int i = 1; i <= 50; ++i) s->sketch_add(h->h, a, (double)i, 1.0);
    for (int i = 51; i <= 100; ++i) s->sketch_add(h->h, b, (double)i, 1.0);
    s->sketch_merge(h->h, a, b);
    *out_count = s->sketch_result(h->h, a).count;
    s->sketch_free(h->h, a);
    s->sketch_free(h->h, b);
    return 0;
}
