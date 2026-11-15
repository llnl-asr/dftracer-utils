// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_C_API_UDF_H
#define DFTRACER_UTILS_DATAFRAME_C_API_UDF_H

/** @file
 * The scalar user-defined function (UDF) description a plugin registers with
 * dftu_svc_ops::register_udf (dftracer/utils/plugins/abi/ops.h), and duql calls
 * by name like a built-in function. A UDF runs one batch at a time over whole
 * columns, never row by row. Include dftracer/utils/plugins/abi.h in a plugin.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The type of one UDF argument or result. Only the scalar column types a UDF
   reads through the dftu_series C functions are accepted: Bool, the integers,
   Float32, Float64, String and Binary. A Bool column is bit-packed, one bit
   per row, least significant bit first. */
typedef struct dftu_type_info {
    uint32_t struct_size; /**< sizeof(dftu_type_info) */
    dftu_dtype id;        /**< a DFTU_TYPE_* code */
} dftu_type_info;

/** dftu_udf_desc::flags. Without a flag the UDF is deterministic and a row
   with a null argument gives a null result. */
enum {
    /** The result can differ for the same arguments. Advisory: the host never
       folds a UDF call to a constant, and never stores a lookup side that
       calls any UDF in the lookup cache, with or without this flag. */
    DFTU_FN_VOLATILE = 1,
    /** The UDF reads null arguments itself. The host passes the columns as
       they are and keeps the result's validity as the UDF made it. Without the
       flag the host nulls the result of every row where an argument is null. */
    DFTU_FN_NULL_AWARE = 2
};

/** A scalar UDF. The host copies the record and the name at registration, so
   the plugin may free its own copy. `self` of the registration is passed to
   bind. The host calls bind once per call site, with the argument types, and
   once more per batch type for an argument whose type only each batch gives;
   exec then runs on many batches, possibly from several threads at once, so
   exec reads `bound` and writes only what it owns. A UDF that keeps mutable
   state in `bound` must guard it. free_bound runs once per successful bind,
   after the last exec of it. */
typedef struct dftu_udf_desc {
    uint32_t struct_size; /**< sizeof(dftu_udf_desc) */
    /** The duql name, spelled as the query writes it: letters, digits and `_`,
       dots between parts allowed. A name of a built-in function or of another
       UDF fails the load with a message. */
    const char* name;
    uint32_t flags;   /**< DFTU_FN_* */
    int32_t min_args; /**< at least 0 */
    int32_t max_args; /**< at least min_args */
    /** Pick the result type and keep the state exec needs. `args` are the `n`
       argument types. `consts[i]` is the value of argument i when the call
       writes a constant there, else NULL; the scalars are borrowed for the
       call only. Set `*out` (its struct_size too) and `*bound`, which may stay
       NULL. A failure fails the query with the message set on `ctx`; the host
       then ignores `*bound` and does not call free_bound, so a failing bind
       releases what it allocated itself. */
    dftu_status (*bind)(dftu_ctx* ctx, void* self, const dftu_type_info* args,
                        uint32_t n, const dftu_scalar* const* consts,
                        dftu_type_info* out, void** bound);
    /** One batch: `n` argument columns of `rows` rows each, borrowed, FLAT or
       string columns that dftu_series_data and dftu_series_offsets read. Set
       `*out` to a new column of `rows` rows and the bound result type, owned
       by the host from then on. Poll dftu_check in a long loop and return its
       status: a cancelled query stops within one batch. Without
       DFTU_FN_NULL_AWARE the host replaces the result of a row with a null
       argument by null, so the UDF may compute anything for that row. */
    dftu_status (*exec)(dftu_ctx* ctx, void* bound,
                        const dftu_series* const* args, uint32_t n,
                        uint64_t rows, dftu_series** out);
    /** Releases `bound` of a successful bind, once; NULL when bind keeps no
       state. */
    void (*free_bound)(void* bound);
} dftu_udf_desc;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_DATAFRAME_C_API_UDF_H */
