// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_TASK_H
#define DFTRACER_UTILS_PLUGINS_ABI_TASK_H

/** @file
 * dftu.svc.task: async work for a plugin. A job is a typed recipe (an input
 * value in, an output value out) that can be piped, fanned out and raced; a
 * task is one run of a job. Fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_TASK). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_TASK "dftu.svc.task"

/** Where a job runs. */
typedef int32_t dftu_job_where;
enum {
    DFTU_JOB_CPU = 0,     /**< on a pool worker */
    DFTU_JOB_INLINE = 1,  /**< on the thread that reaches it; cheap work only */
    DFTU_JOB_BLOCKING = 2 /**< on a pool worker that gives back its run permit
                              while the job runs, for a call that blocks */
};

/** A leaf job body. Reads `in`, writes `out` (both sized by the job's
   descriptor and valid until the job finishes). To finish later, start a task
   through this service and set `*pending` to it; the job then finishes with
   that task's status, and `out` must be written by then. */
typedef dftu_status (*dftu_job_fn)(dftu_ctx* ctx, void* state, const void* in,
                                   void* out, dftu_task** pending);

typedef struct dftu_job_desc {
    uint32_t struct_size;
    dftu_job_where where;
    dftu_job_fn fn;
    void* state;
    void (*free_state)(void* state); /**< NULL when state needs no free */
    dftu_type in_ty;  /**< DFTU_T_VOID with in_size 0 for no input */
    uint32_t in_size;
    dftu_type out_ty; /**< DFTU_T_VOID with out_size 0 for no output */
    uint32_t out_size;
} dftu_job_desc;

/** The input of a map job: `n` values of the body's in_size, packed. */
typedef struct dftu_job_items {
    const void* items;
    uint64_t n;
} dftu_job_items;

/** The output of a map job: room for `n` values of the body's out_size. */
typedef struct dftu_job_slots {
    void* items;
    uint64_t n;
} dftu_job_slots;

/** A drive step: start the tasks to wait for, put them in `wait` (room for
   `cap`) and set `*n_wait`. The host awaits all of them, then calls the step
   again. Return DFTU_END when done, or an error. The tasks of the previous
   round stay readable through dftu_svc_task::status until the step returns. */
typedef dftu_status (*dftu_step_fn)(dftu_ctx* ctx, void* coro, dftu_task** wait,
                                    uint32_t cap, uint32_t* n_wait);

/**
 * Jobs and tasks. Jobs live until the fold ends. A task is owned by whoever
 * awaits it: the host, once the task is returned through a `pending` or `wait`
 * slot. The plugin must hand back or discard every task it starts, also on
 * an error return; a task it drops leaks. Every combinator cancels and awaits
 * the children it no longer needs before it finishes, so none runs after it.
 */
typedef struct dftu_svc_task {
    uint32_t struct_size;
    /** A leaf job; the descriptor is copied. */
    dftu_status (*job)(dftu_ctx* ctx, void* h, const dftu_job_desc* desc,
                       dftu_job** out);
    /** A job that waits `ns` nanoseconds and then fails with DFTU_E_TIMEOUT,
       for a deadline under `any`. No input, no output. */
    dftu_status (*sleep)(dftu_ctx* ctx, void* h, uint64_t ns, dftu_job** out);
    /** Pipe: run `a`, feed its output to `b`. out of `a` must match in of `b`
       by type and size (DFTU_E_INVALID otherwise). */
    dftu_status (*then)(dftu_ctx* ctx, void* h, dftu_job* a, dftu_job* b,
                        dftu_job** out);
    /** Fan out and in: run all `n` jobs on the same input at once; the output
       is their outputs concatenated in order. The first error cancels the
       rest and is the result. */
    dftu_status (*all)(dftu_ctx* ctx, void* h, dftu_job* const* ops, uint32_t n,
                       dftu_job** out);
    /** Race: run all `n` jobs on the same input; the first job to finish
       decides (its output, or its error) and the rest are cancelled. With a
       sleep job among them it is a deadline. The jobs share one input and
       one output type and size. */
    dftu_status (*any)(dftu_ctx* ctx, void* h, dftu_job* const* ops, uint32_t n,
                       dftu_job** out);
    /** Data parallel: input a dftu_job_items, output a dftu_job_slots of the
       same n; runs `body` once per item with at most `max_parallel` at once
       (0 = the pool size). The first error cancels the rest. */
    dftu_status (*map)(dftu_ctx* ctx, void* h, dftu_job* body,
                       uint32_t max_parallel, dftu_job** out);
    /** Start `job` on `in`, writing `out`; both must stay valid until the
       task finishes. */
    dftu_status (*start)(dftu_ctx* ctx, void* h, dftu_job* job, const void* in,
                         void* out, dftu_task** out_task);
    /** Start fn(arg) with no job: the short form of a no-input, no-output
       leaf. */
    dftu_status (*spawn)(dftu_ctx* ctx, void* h,
                         dftu_status (*fn)(dftu_ctx* ctx, void* arg), void* arg,
                         dftu_job_where where, dftu_task** out_task);
    /** A task that runs `step` in rounds, for a fan-in whose count is known
       only at run time. Once drive succeeds, the host calls release(coro)
       exactly once when the drive ends for any reason (DFTU_END, an error, a
       cancel, or the task freed unawaited), and never calls step after it.
       `release` may be NULL. When drive fails, `coro` stays the plugin's. */
    dftu_status (*drive)(dftu_ctx* ctx, void* h, dftu_step_fn step, void* coro,
                         void (*release)(void* coro), dftu_task** out_task);
    /** The status of a task the host awaited for the current step; a
       negative status also sets its message on `ctx`. DFTU_E_INVALID for a
       task that has not finished. */
    dftu_status (*status)(dftu_ctx* ctx, void* h, const dftu_task* t);
    /** Give back a task the plugin started but will not hand to the host,
       such as on an error path; the task never runs. A task already handed
       back must not be discarded. */
    void (*discard)(void* h, dftu_task* t);
} dftu_svc_task;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_TASK_H */
