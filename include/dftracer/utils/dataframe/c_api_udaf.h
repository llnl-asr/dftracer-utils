// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_C_API_UDAF_H
#define DFTRACER_UTILS_DATAFRAME_C_API_UDAF_H

/** @file
 * The user-defined aggregate function (UDAF) description a plugin registers
 * with dftu_svc_ops::register_udaf (dftracer/utils/plugins/abi/ops.h). A UDAF
 * keeps one state per group. The host owns the memory of every state, hands
 * the plugin whole arrays of state pointers, and runs update, combine and
 * finalize over columns, never row by row. Include
 * dftracer/utils/plugins/abi.h in a plugin.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/c_api_udf.h> /* dftu_type_info */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** dftu_udaf_desc::flags. Without a flag the state is not plain bytes, one
   thread feeds a state set, and a row with a null argument is skipped. */
enum {
    /** The state is plain bytes. The host zero-fills a state before init, and
       may copy it with memcpy to serialize, spill or move it, so serialize,
       deserialize and destroy are not needed. */
    DFTU_AGG_STATE_POD = 1,
    /** Several state sets of the UDAF may be fed on different threads at once
       and merged with combine. A set itself is never used by two threads at
       once. Advisory: the host decides whether to split a group by. Without
       the flag no callback of the UDAF runs on two threads at once, bind,
       init, destroy, serialize, deserialize and free_bound included. */
    DFTU_AGG_PARALLEL_SAFE = 2,
    /** The UDAF provides retract and value, so a window can slide its frame. */
    DFTU_AGG_HAS_RETRACT = 4,
    /** The UDAF reads null arguments itself: update sees every row. Without
       the flag the host passes a NULL state pointer for a row where any
       argument is null, so the plugin skips it. */
    DFTU_AGG_NULL_AWARE = 8
};

/** A user-defined aggregate. The host copies the record and the name at
   registration, so the plugin may free its own copy. `self` of the
   registration is passed to bind. bind runs with the argument types for a
   group-by, and again for each state set the host builds apart from the
   first, such as a partial fed on another thread or states read back from a
   spill; the other callbacks then run on the `bound` of the bind that made
   their states. A `bound` is shared by every state set copied or merged from
   its own, so its callbacks may run for several state sets.
   Every callback that gets `states` gets pointers to states of this UDAF only,
   each `state_size` bytes at `state_align`, valid until the host destroys
   them. A state never moves while it lives, unless the UDAF is
   DFTU_AGG_STATE_POD. One state set is used by one thread at a time. */
typedef struct dftu_udaf_desc {
    uint32_t struct_size; /**< sizeof(dftu_udaf_desc) */
    /** The duql name, spelled as the query writes it: letters, digits and `_`,
       dots between parts allowed. A name of a built-in function or aggregate,
       an op, a UDF or another UDAF fails the load with a message. */
    const char* name;
    uint32_t flags;   /**< DFTU_AGG_* */
    int32_t min_args; /**< at least 0 */
    int32_t max_args; /**< at least min_args */
    /** Size in bytes of one state, at least 1. */
    uint32_t state_size;
    /** Alignment of one state: a power of two, at most 64. */
    uint32_t state_align;
    /** Pick the result type and keep what the other callbacks need. `args` are
       the `n` argument types. `consts[i]` is the value of argument i when the
       call writes a constant there, else NULL; the scalars are borrowed for the
       call only. Set `*out` (its struct_size too) and `*bound`, which may stay
       NULL. A failure fails the query with the message set on `ctx`; the host
       then ignores `*bound` and does not call free_bound. */
    dftu_status (*bind)(dftu_ctx* ctx, void* self, const dftu_type_info* args,
                        uint32_t n, const dftu_scalar* const* consts,
                        dftu_type_info* out, void** bound);
    /** Makes `n` new states ready: `states[i]` points at zero-filled memory.
       Cannot fail. */
    void (*init)(void* bound, void* const* states, uint64_t n);
    /** Feeds one batch: `args` are `nargs` columns of `rows` rows, borrowed,
       and `states[i]` is the state of the group of row i. `states[i]` is NULL
       for any row the host skips, in every mode, with DFTU_AGG_NULL_AWARE too;
       the plugin skips it. Without DFTU_AGG_NULL_AWARE a row with a null
       argument is skipped this way. Several rows may name one state; apply them
       in order. Poll dftu_check in a long loop and return its status. */
    dftu_status (*update)(dftu_ctx* ctx, void* bound, void* const* states,
                          const dftu_series* const* args, uint32_t nargs,
                          uint64_t rows);
    /** Merges `from[i]` into `into[i]` for i below `n`. The same `into` state
       may appear more than once; apply the pairs in order. combine only reads
       a `from` state and leaves it unchanged: a window merges one `from` into
       many frames. In a window, `from` holds rows that come after the rows of
       `into`. */
    dftu_status (*combine)(dftu_ctx* ctx, void* bound, void* const* into,
                           void* const* from, uint64_t n);
    /** Gives the result of each of the `n` states, in order: set `*out` to a
       new column of `n` rows of the bound result type, owned by the host from
       then on. The states stay valid and are not changed. */
    dftu_status (*finalize)(dftu_ctx* ctx, void* bound, void* const* states,
                            uint64_t n, dftu_series** out);
    /** Releases what `n` states hold. Required unless DFTU_AGG_STATE_POD; the
       host never frees the state memory itself through it. Cannot fail. */
    void (*destroy)(void* bound, void* const* states, uint64_t n);
    /** Required unless DFTU_AGG_STATE_POD: writes `state` as bytes, for a
       spill and a merge across processes. Two-call size query: when `out` is
       NULL or `cap` is below the size needed, set `*size` to the size needed,
       return DFTU_OK and write nothing. Else write that many bytes to `out` and
       set `*size` to it. */
    dftu_status (*serialize)(dftu_ctx* ctx, void* bound, const void* state,
                             uint8_t* out, uint64_t cap, uint64_t* size);
    /** Required with serialize: reads `n` bytes it wrote into `state`, a state
       that init made and nothing has fed. */
    dftu_status (*deserialize)(dftu_ctx* ctx, void* bound, const uint8_t* in,
                               uint64_t n, void* state);
    /** Optional, with DFTU_AGG_HAS_RETRACT: takes rows out of the states, as
       update puts them in. */
    dftu_status (*retract)(dftu_ctx* ctx, void* bound, void* const* states,
                           const dftu_series* const* args, uint32_t nargs,
                           uint64_t rows);
    /** Optional, with DFTU_AGG_HAS_RETRACT: the result of one state as a
       scalar, set in `*out`. A STR scalar stays valid until the next call. */
    dftu_status (*value)(dftu_ctx* ctx, void* bound, const void* state,
                         dftu_scalar* out);
    /** Releases `bound` of a successful bind, once, after the last use of it
       and of every state; NULL when bind keeps no state. */
    void (*free_bound)(void* bound);
} dftu_udaf_desc;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_DATAFRAME_C_API_UDAF_H */
