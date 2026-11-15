// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_memory ABI. The calls are synchronous, so
 * the scenario runs them in place. The pool gauges and the budget are read from
 * the C++ side, which owns the host. */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MSG_CAP 256

typedef struct {
    dftu_ctx* ctx;
    const dftu_svc_memory* mem;
    void* h;
    const dftu_test_harness* hn;
} scn;

static uint64_t g_reclaimed;
static uint64_t g_want;

static uint64_t reclaim_all(void* ud, uint64_t want) {
    g_want = want;
    g_reclaimed += want;
    (void)ud;
    return want;
}

static void check_blocks(scn* s) {
    const dftu_svc_memory* m = s->mem;
    unsigned char* p = (unsigned char*)m->alloc(s->h, 100, 16);
    s->hn->expect("alloc gives a block", p != NULL);
    if (!p) return;
    s->hn->expect("the block has the alignment", ((uintptr_t)p % 16) == 0);
    memset(p, 0x5a, 100);
    unsigned char* q = (unsigned char*)m->realloc(s->h, p, 100, 5000, 16);
    s->hn->expect("realloc gives a block", q != NULL);
    if (!q) {
        m->free(s->h, p, 100, 16);
        return;
    }
    s->hn->expect("realloc keeps the bytes", q[0] == 0x5a && q[99] == 0x5a);
    unsigned char* r = (unsigned char*)m->realloc(s->h, q, 5000, 10, 16);
    s->hn->expect("realloc shrinks and keeps the prefix",
                  r != NULL && r[9] == 0x5a);
    if (r)
        m->free(s->h, r, 10, 16);
    else
        m->free(s->h, q, 5000, 16);

    unsigned char* fresh = (unsigned char*)m->realloc(s->h, NULL, 0, 64, 8);
    s->hn->expect("realloc of NULL is alloc", fresh != NULL);
    m->free(s->h, fresh, 64, 8);
    m->free(s->h, NULL, 0, 8);

    s->hn->expect("an alignment that is not a power of two gives NULL",
                  m->alloc(s->h, 64, 24) == NULL);
    s->hn->expect("a zero alignment gives NULL", m->alloc(s->h, 64, 0) == NULL);
    s->hn->expect("an alignment over 4096 gives NULL",
                  m->alloc(s->h, 64, 8192) == NULL);
}

static void check_reservation(scn* s) {
    const dftu_svc_memory* m = s->mem;
    char msg[MSG_CAP];
    dftu_reservation* r = NULL;
    s->hn->expect("reserve opens a reservation",
                  m->reserve(s->ctx, s->h, &r, "c consumer") == DFTU_OK && r);
    if (!r) return;
    s->hn->expect("a grow fits", m->try_grow(s->ctx, r, 1000) == DFTU_OK);
    s->hn->expect("a grow of zero is nothing",
                  m->try_grow(s->ctx, r, 0) == DFTU_OK);
    m->shrink(r, 400);
    m->shrink(r, 100000);
    s->hn->expect("a grow fits after a shrink",
                  m->try_grow(s->ctx, r, 10) == DFTU_OK);

    const dftu_status st = m->try_grow(s->ctx, r, UINT64_C(1) << 62);
    s->hn->expect("a grow past the budget is DFTU_E_NO_MEMORY",
                  st == DFTU_E_NO_MEMORY);
    msg[0] = 0;
    s->hn->ctx_message(s->ctx, msg, MSG_CAP);
    s->hn->expect("the refusal has a message",
                  strstr(msg, "c consumer") != NULL);
    s->hn->expect("the reservation still grows after a refusal",
                  m->try_grow(s->ctx, r, 10) == DFTU_OK);

    g_reclaimed = 0;
    g_want = 0;
    m->set_reclaimer(r, reclaim_all, NULL);
    s->hn->expect(
        "a grow no reclaim can make room for still fails",
        m->try_grow(s->ctx, r, UINT64_C(1) << 62) == DFTU_E_NO_MEMORY);
    s->hn->expect("the reclaimer was asked for bytes", g_want != 0);
    m->set_reclaimer(r, NULL, NULL);
    m->release(r);
    m->release(NULL);
}

void dftu_test_memory_ops(dftu_ctx* ctx, const dftu_plugin_host* h,
                          const dftu_test_harness* hn) {
    scn sc = {ctx,
              (const dftu_svc_memory*)h->get_service(h->h, DFTU_SVC_MEMORY),
              h->h, hn};
    scn* s = &sc;
    hn->expect("the service is served", s->mem != NULL);
    if (!s->mem) return;
    hn->expect("the table has its size",
               s->mem->struct_size == sizeof(dftu_svc_memory));
    check_blocks(s);
    check_reservation(s);
}
