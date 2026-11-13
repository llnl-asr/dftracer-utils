// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_task ABI: this translation unit is compiled
 * by the C compiler and sees only <dftracer/utils/plugins/abi.h>, so it proves
 * the task service is callable from C (no C++ leakage). The C++ test harness
 * builds the host, passes a ctx, and drives the tasks these functions return.
 * Every function builds jobs, starts one task and hands it back through *t,
 * returning the status of building and starting. */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static const dftu_svc_task* task_of(const dftu_plugin_host* h) {
    return (const dftu_svc_task*)h->get_service(h->h, DFTU_SVC_TASK);
}

static dftu_status leaf(dftu_ctx* ctx, const dftu_plugin_host* h,
                        dftu_job_fn fn, void* state, dftu_job_where where,
                        dftu_type in_ty, uint32_t in_size, dftu_type out_ty,
                        uint32_t out_size, dftu_job** out) {
    dftu_job_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.where = where;
    d.fn = fn;
    d.state = state;
    d.in_ty = in_ty;
    d.in_size = in_size;
    d.out_ty = out_ty;
    d.out_size = out_size;
    return task_of(h)->job(ctx, h->h, &d, out);
}

static dftu_status leaf_i64(dftu_ctx* ctx, const dftu_plugin_host* h,
                            dftu_job_fn fn, void* state, dftu_job** out) {
    return leaf(ctx, h, fn, state, DFTU_JOB_INLINE, DFTU_T_I64, 8, DFTU_T_I64,
                8, out);
}

static dftu_status c_double(dftu_ctx* ctx, void* state, const void* in,
                            void* out, dftu_task** pending) {
    (void)ctx;
    (void)state;
    (void)pending;
    *(int64_t*)out = *(const int64_t*)in * 2;
    return DFTU_OK;
}

static dftu_status c_plus10(dftu_ctx* ctx, void* state, const void* in,
                            void* out, dftu_task** pending) {
    (void)ctx;
    (void)state;
    (void)pending;
    *(int64_t*)out = *(const int64_t*)in + 10;
    return DFTU_OK;
}

static atomic_int g_waiter_started;

/* A job that must not decide before the waiter runs, or the waiter would be
 * cancelled before its first dftu_check. */
static void wait_for_waiter(void) {
    int i;
    for (i = 0; i < 1000 && !atomic_load(&g_waiter_started); ++i) usleep(1000);
}

static dftu_status c_fail(dftu_ctx* ctx, void* state, const void* in, void* out,
                          dftu_task** pending) {
    (void)state;
    (void)in;
    (void)out;
    (void)pending;
    wait_for_waiter();
    return dftu_fail(ctx, DFTU_E_PARSE, "c_fail: refused on purpose");
}

static dftu_status c_plus10_late(dftu_ctx* ctx, void* state, const void* in,
                                 void* out, dftu_task** pending) {
    wait_for_waiter();
    return c_plus10(ctx, state, in, out, pending);
}

/* Run it as a CPU job: an inline job would hold the thread that starts its
 * siblings. Waits (up to two seconds) for a cancel to reach it through
 * dftu_check, and records that it did in *state. */
static dftu_status c_wait_cancel(dftu_ctx* ctx, void* state, const void* in,
                                 void* out, dftu_task** pending) {
    int i;
    (void)in;
    (void)out;
    (void)pending;
    atomic_store(&g_waiter_started, 1);
    for (i = 0; i < 2000; ++i) {
        const dftu_status st = dftu_check(ctx);
        if (st < 0) {
            *(volatile int*)state = 1;
            return st;
        }
        usleep(1000);
    }
    return DFTU_OK;
}

static dftu_task* start(dftu_ctx* ctx, const dftu_plugin_host* h, dftu_job* job,
                        const void* in, void* out, dftu_task** t,
                        dftu_status* st) {
    *st = task_of(h)->start(ctx, h->h, job, in, out, t);
    return *t;
}

/* (in*2) then (+10). */
dftu_status dftu_test_task_pipe(dftu_ctx* ctx, const dftu_plugin_host* h,
                                const int64_t* in, int64_t* out,
                                dftu_task** t) {
    dftu_job *a, *b, *p;
    dftu_status st = leaf_i64(ctx, h, c_double, NULL, &a);
    if (st < 0) return st;
    st = leaf_i64(ctx, h, c_plus10, NULL, &b);
    if (st < 0) return st;
    st = task_of(h)->then(ctx, h->h, a, b, &p);
    if (st < 0) return st;
    start(ctx, h, p, in, out, t, &st);
    return st;
}

/* all fans one input to both leaves; out is int64[2] = {in*2, in+10}. */
dftu_status dftu_test_task_all(dftu_ctx* ctx, const dftu_plugin_host* h,
                               const int64_t* in, int64_t* out, dftu_task** t) {
    dftu_job* ops[2];
    dftu_job* all;
    dftu_status st = leaf_i64(ctx, h, c_double, NULL, &ops[0]);
    if (st < 0) return st;
    st = leaf_i64(ctx, h, c_plus10, NULL, &ops[1]);
    if (st < 0) return st;
    st = task_of(h)->all(ctx, h->h, ops, 2, &all);
    if (st < 0) return st;
    start(ctx, h, all, in, out, t, &st);
    return st;
}

/* The ABI rejects a mismatched pipe (i64 -> f64) with *mismatch set to its
 * status, and accepts a matching one (i64 -> i64); returns the matching
 * pipe's status. Pure graph building. */
dftu_status dftu_test_task_typecheck(dftu_ctx* ctx, const dftu_plugin_host* h,
                                     dftu_status* mismatch) {
    dftu_job *a, *f, *i, *p;
    dftu_status st = leaf_i64(ctx, h, c_double, NULL, &a);
    if (st < 0) return st;
    st = leaf(ctx, h, c_plus10, NULL, DFTU_JOB_INLINE, DFTU_T_F64, 8,
              DFTU_T_F64, 8, &f);
    if (st < 0) return st;
    st = leaf_i64(ctx, h, c_plus10, NULL, &i);
    if (st < 0) return st;
    *mismatch = task_of(h)->then(ctx, h->h, a, f, &p);
    return task_of(h)->then(ctx, h->h, a, i, &p);
}

/* all(fail, wait): the first error cancels the other, which sees it through
 * dftu_check (*saw_cancel set) and the task ends with the failing job's
 * status and message. */
dftu_status dftu_test_task_all_error(dftu_ctx* ctx, const dftu_plugin_host* h,
                                     const int64_t* in, int64_t* out,
                                     int* saw_cancel, dftu_task** t) {
    dftu_job* ops[2];
    dftu_job* all;
    dftu_status st;
    atomic_store(&g_waiter_started, 0);
    st = leaf(ctx, h, c_wait_cancel, saw_cancel, DFTU_JOB_CPU, DFTU_T_I64, 8,
              DFTU_T_I64, 8, &ops[0]);
    if (st < 0) return st;
    st = leaf_i64(ctx, h, c_fail, NULL, &ops[1]);
    if (st < 0) return st;
    st = task_of(h)->all(ctx, h->h, ops, 2, &all);
    if (st < 0) return st;
    start(ctx, h, all, in, out, t, &st);
    return st;
}

/* any(wait, plus10): the first to finish decides, so out = in+10, and the
 * waiter is cancelled (*saw_cancel). */
dftu_status dftu_test_task_any_first(dftu_ctx* ctx, const dftu_plugin_host* h,
                                     const int64_t* in, int64_t* out,
                                     int* saw_cancel, dftu_task** t) {
    dftu_job* ops[2];
    dftu_job* any;
    dftu_status st;
    atomic_store(&g_waiter_started, 0);
    st = leaf(ctx, h, c_wait_cancel, saw_cancel, DFTU_JOB_CPU, DFTU_T_I64, 8,
              DFTU_T_I64, 8, &ops[0]);
    if (st < 0) return st;
    st = leaf_i64(ctx, h, c_plus10_late, NULL, &ops[1]);
    if (st < 0) return st;
    st = task_of(h)->any(ctx, h->h, ops, 2, &any);
    if (st < 0) return st;
    start(ctx, h, any, in, out, t, &st);
    return st;
}

/* any(wait, sleep(200 ms)) with no input or output: the deadline wins and the
 * task ends with DFTU_E_TIMEOUT; the waiter is cancelled. */
dftu_status dftu_test_task_any_deadline(dftu_ctx* ctx,
                                        const dftu_plugin_host* h,
                                        int* saw_cancel, dftu_task** t) {
    dftu_job* ops[2];
    dftu_job* any;
    dftu_status st = leaf(ctx, h, c_wait_cancel, saw_cancel, DFTU_JOB_CPU,
                          DFTU_T_VOID, 0, DFTU_T_VOID, 0, &ops[0]);
    atomic_store(&g_waiter_started, 0);
    if (st < 0) return st;
    st = task_of(h)->sleep(ctx, h->h, 200ull * 1000 * 1000, &ops[1]);
    if (st < 0) return st;
    st = task_of(h)->any(ctx, h->h, ops, 2, &any);
    if (st < 0) return st;
    start(ctx, h, any, NULL, NULL, t, &st);
    return st;
}

static atomic_int g_running;
static atomic_int g_max_running;
static int64_t g_fail_item;
static dftu_job_items g_items;
static dftu_job_slots g_slots;

/* Doubles an item, tracking how many bodies run at once; fails on g_fail_item
 * after the others had a chance to start. */
static dftu_status c_map_body(dftu_ctx* ctx, void* state, const void* in,
                              void* out, dftu_task** pending) {
    int now = atomic_fetch_add(&g_running, 1) + 1;
    int seen = atomic_load(&g_max_running);
    dftu_status st = DFTU_OK;
    (void)state;
    (void)pending;
    while (now > seen &&
           !atomic_compare_exchange_weak(&g_max_running, &seen, now)) {
    }
    usleep(3000);
    if (*(const int64_t*)in == g_fail_item)
        st = dftu_fail(ctx, DFTU_E_INVALID, "c_map_body: item refused");
    else
        *(int64_t*)out = *(const int64_t*)in * 2;
    atomic_fetch_sub(&g_running, 1);
    return st;
}

/* map over n items with at most max_parallel at once; *max_seen reports the
 * most bodies that ran together. fail_item < 0 means none fails. */
dftu_status dftu_test_task_map(dftu_ctx* ctx, const dftu_plugin_host* h,
                               const int64_t* items, int64_t* results,
                               uint64_t n, uint32_t max_parallel,
                               int64_t fail_item, int* max_seen,
                               dftu_task** t) {
    dftu_job *body, *map;
    dftu_status st;
    (void)max_seen;
    atomic_store(&g_running, 0);
    atomic_store(&g_max_running, 0);
    g_fail_item = fail_item;
    g_items.items = items;
    g_items.n = n;
    g_slots.items = results;
    g_slots.n = n;
    st = leaf_i64(ctx, h, c_map_body, NULL, &body);
    if (st < 0) return st;
    st = task_of(h)->map(ctx, h->h, body, max_parallel, &map);
    if (st < 0) return st;
    start(ctx, h, map, &g_items, &g_slots, t, &st);
    return st;
}

int dftu_test_task_map_max_seen(void) { return atomic_load(&g_max_running); }

static dftu_status spawned(dftu_ctx* ctx, void* arg) {
    dftu_spawn_probe* p = (dftu_spawn_probe*)arg;
    p->ran += 1;
    if (p->fail_with < 0) return dftu_fail(ctx, p->fail_with, "spawned failed");
    return DFTU_OK;
}

/* spawn(fn) where the function counts its runs and returns fail_with. */
dftu_status dftu_test_task_spawn(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 dftu_spawn_probe* probe, dftu_job_where where,
                                 dftu_task** t) {
    return task_of(h)->spawn(ctx, h->h, spawned, probe, where, t);
}

typedef struct {
    const dftu_plugin_host* h;
    dftu_drive_report* rep;
    dftu_spawn_probe ok_probe;
    dftu_spawn_probe bad_probe;
    dftu_task* last;
    int round;
} DriveState;

static DriveState g_drive;

/* Round 0 starts a task that succeeds; round 1 reads its status and starts one
 * that fails; round 2 reads the failure through status(). With propagate the
 * step returns that status, so the drive task fails with the message. */
static dftu_status drive_step(dftu_ctx* ctx, void* coro, dftu_task** wait,
                              uint32_t cap, uint32_t* n_wait) {
    DriveState* d = (DriveState*)coro;
    const dftu_svc_task* tk = task_of(d->h);
    dftu_status st;
    (void)cap;
    d->rep->steps += 1;
    if (d->rep->empty_round) {
        *n_wait = 0;
        return DFTU_OK;
    }
    if (d->round == 0) {
        st = tk->spawn(ctx, d->h->h, spawned, &d->ok_probe, DFTU_JOB_CPU,
                       &d->last);
        if (st < 0) return st;
    } else if (d->round == 1) {
        d->rep->first_status = tk->status(ctx, d->h->h, d->last);
        st = tk->spawn(ctx, d->h->h, spawned, &d->bad_probe, DFTU_JOB_CPU,
                       &d->last);
        if (st < 0) return st;
    } else {
        d->rep->second_status = tk->status(ctx, d->h->h, d->last);
        return d->rep->propagate ? d->rep->second_status : DFTU_END;
    }
    d->round += 1;
    wait[0] = d->last;
    *n_wait = 1;
    return DFTU_OK;
}

static void drive_release(void* coro) {
    ((DriveState*)coro)->rep->released += 1;
}

dftu_status dftu_test_task_drive(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 dftu_drive_report* rep, dftu_task** t) {
    memset(&g_drive, 0, sizeof(g_drive));
    g_drive.h = h;
    g_drive.rep = rep;
    g_drive.bad_probe.fail_with = DFTU_E_IO;
    return task_of(h)->drive(ctx, h->h, drive_step, &g_drive, drive_release, t);
}
