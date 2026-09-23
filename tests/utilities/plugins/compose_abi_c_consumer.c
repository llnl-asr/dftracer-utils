/* A real C consumer of the dftu_svc_compose ABI: this translation unit is
 * compiled by the C compiler and sees only <dftracer/utils/plugins/abi.h>, so
 * it proves the compose ABI is callable from C (no C++ leakage). The C++ test
 * harness builds the host and drives the tasks these functions return. */
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

static dftu_task* c_double(void* state, const void* in, void* out, int* rc) {
    (void)state;
    *(int64_t*)out = *(const int64_t*)in * 2;
    if (rc) *rc = 0;
    return NULL; /* ran inline */
}

static dftu_task* c_plus10(void* state, const void* in, void* out, int* rc) {
    (void)state;
    *(int64_t*)out = *(const int64_t*)in + 10;
    if (rc) *rc = 0;
    return NULL;
}

static const dftu_svc_compose* compose_of(const dftu_plugin_host* h) {
    return (const dftu_svc_compose*)h->get_service(h->h, DFTU_SVC_COMPOSE);
}

/* (in*2) then (+10); returns the run task, writes *out and *rc. */
dftu_task* dftu_test_compose_pipe(const dftu_plugin_host* h, const int64_t* in,
                                  int64_t* out, int* rc) {
    const dftu_svc_compose* c = compose_of(h);
    if (!c) return NULL;
    dftu_op* a =
        c->make_op(h->h, c_double, NULL, NULL, DFTU_T_I64, 8, DFTU_T_I64, 8);
    dftu_op* b =
        c->make_op(h->h, c_plus10, NULL, NULL, DFTU_T_I64, 8, DFTU_T_I64, 8);
    return c->run(h->h, c->then(h->h, a, b), in, out, rc);
}

/* when_all fans one input to both leaves; out is int64[2] = {in*2, in+10}. */
dftu_task* dftu_test_compose_all(const dftu_plugin_host* h, const int64_t* in,
                                 int64_t* out, int* rc) {
    const dftu_svc_compose* c = compose_of(h);
    if (!c) return NULL;
    dftu_op* ops[2];
    ops[0] =
        c->make_op(h->h, c_double, NULL, NULL, DFTU_T_I64, 8, DFTU_T_I64, 8);
    ops[1] =
        c->make_op(h->h, c_plus10, NULL, NULL, DFTU_T_I64, 8, DFTU_T_I64, 8);
    return c->run(h->h, c->when_all(h->h, ops, 2), in, out, rc);
}

/* 1 if the ABI rejects a mismatched pipe (i64 -> f64) and accepts a matching
 * one (i64 -> i64); pure graph building, no run needed. */
int dftu_test_compose_typecheck(const dftu_plugin_host* h) {
    const dftu_svc_compose* c = compose_of(h);
    if (!c) return 0;
    dftu_op* a =
        c->make_op(h->h, c_double, NULL, NULL, DFTU_T_I64, 8, DFTU_T_I64, 8);
    dftu_op* f =
        c->make_op(h->h, c_plus10, NULL, NULL, DFTU_T_F64, 8, DFTU_T_F64, 8);
    dftu_op* i =
        c->make_op(h->h, c_plus10, NULL, NULL, DFTU_T_I64, 8, DFTU_T_I64, 8);
    return c->then(h->h, a, f) == NULL && c->then(h->h, a, i) != NULL;
}
