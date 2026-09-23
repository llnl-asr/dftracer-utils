#ifndef DFTRACER_UTILS_PLUGINS_ABI_CORE_H
#define DFTRACER_UTILS_PLUGINS_ABI_CORE_H

/** @file
 * Fundamental types shared by every plugin ABI service group: value types,
 * opaque handles, the error/result channel, and the host descriptor. Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/core/common/abi.h>
#include <dftracer/utils/core/common/export.h>
#include <dftracer/utils/plugins/abi_version.h> /* DFTRACER_PLUGIN_ABI_VERSION */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The standard Arrow C Data Interface structs (defined in arrow_abi.h); only
   opaque pointers to them cross this ABI. */
struct ArrowArray;
struct ArrowSchema;

/** Interned string id; resolve for bytes stable for the whole scan.
   DFTU_STR_NONE marks an absent field. */
typedef uint32_t dftu_str;
#define DFTU_STR_NONE ((dftu_str)0xFFFFFFFFu)

typedef enum {
    DFTU_T_VOID = 0,
    DFTU_T_F64,
    DFTU_T_I64,
    DFTU_T_STR,
    DFTU_T_BYTES,
    DFTU_T_TABLE,
    /** Fixed-width map-key/column scalar types; appended, never renumbered. */
    DFTU_T_I8,
    DFTU_T_I16,
    DFTU_T_I32,
    DFTU_T_U8,
    DFTU_T_U16,
    DFTU_T_U32,
    DFTU_T_U64,
    /** Float key components are bit-cast into the int64 key slot (exact bit
       equality) and materialize as float32/float64; F64 reuses its enumerator
       above for an f64 key. */
    DFTU_T_F32,
    /** A column handle carried by value: the compose value is a `dftu_series*`
       pointer (size = sizeof(pointer)), passed zero-copy - only the handle
       moves, never the column data. Ownership moves to the receiver unless the
       op documents a borrow; the final owner frees with `dftu_series_free`.
       DFTU_T_TABLE is the matching tag for a `dftu_dataframe*` handle. */
    DFTU_T_SERIES
} dftu_type;

/** ABI-stable phase values; the host adapter maps these to the internal enum.
 */
typedef enum {
    DFTU_PH_UNKNOWN = 0,
    DFTU_PH_COMPLETE,
    DFTU_PH_COUNTER,
    DFTU_PH_AGGREGATED,
    DFTU_PH_METADATA
} dftu_phase;

typedef struct dftu_plugin_host dftu_plugin_host;
/** async node; awaited exactly once */
#ifndef DFTU_TYPEDEF_DFTU_TASK
#define DFTU_TYPEDEF_DFTU_TASK
typedef struct dftu_task dftu_task;
#endif
typedef struct dftu_writer dftu_writer; /**< parallel output; host-owned */
/** compiled filter; scan-lifetime */
#ifndef DFTU_TYPEDEF_DFTU_QUERY
#define DFTU_TYPEDEF_DFTU_QUERY
typedef struct dftu_query dftu_query;
#endif
typedef struct dftu_sketch
    dftu_sketch;                /**< quantile accumulator; plugin-owned */
typedef struct dftu_trace_writer
    dftu_trace_writer;          /**< trace output; host-owned */
typedef struct dftu_op dftu_op; /**< composable async op node; scan-lifetime */
/** A columnar batch/table handle for the vectorized fold seam. The concrete
   type is the dataframe engine's (dftracer/utils/dataframe/abi.h); a plugin
   that uses it includes that header for the dftu_dataframe and dftu_series
   column ops. */
#ifndef DFTU_TYPEDEF_DFTU_DATAFRAME
#define DFTU_TYPEDEF_DFTU_DATAFRAME
typedef struct dftu_dataframe dftu_dataframe;
#endif
/** A deferred columnar plan handle (the lazy counterpart to dftu_dataframe).
   The concrete type is the dataframe engine's (dftracer/utils/dataframe/abi.h).
 */
#ifndef DFTU_TYPEDEF_DFTU_LAZYFRAME
#define DFTU_TYPEDEF_DFTU_LAZYFRAME
typedef struct dftu_lazyframe dftu_lazyframe;
#endif
/** A single column handle for the vectorized fold seam. The concrete type is
   the dataframe engine's (dftracer/utils/dataframe/abi.h); a plugin that uses
   it includes that header for the column ops. */
#ifndef DFTU_TYPEDEF_DFTU_SERIES
#define DFTU_TYPEDEF_DFTU_SERIES
typedef struct dftu_series dftu_series;
#endif
/** A registered dataframe op record and its call-site operand bag. The
   concrete types are the dataframe engine's (dftracer/utils/dataframe/abi.h).
 */
#ifndef DFTU_TYPEDEF_DFTU_OP_DESC
#define DFTU_TYPEDEF_DFTU_OP_DESC
typedef struct dftu_op_desc dftu_op_desc;
#endif
#ifndef DFTU_TYPEDEF_DFTU_OP_ARG
#define DFTU_TYPEDEF_DFTU_OP_ARG
typedef struct dftu_op_arg dftu_op_arg;
#endif
/** A tagged scalar value. The concrete type is the dataframe engine's
   (dftracer/utils/dataframe/abi.h); a plugin that reads its fields includes
   that header. Passed by pointer here so this header need not define it. */
#ifndef DFTU_TYPEDEF_DFTU_SCALAR
#define DFTU_TYPEDEF_DFTU_SCALAR
typedef struct dftu_scalar dftu_scalar;
#endif

DFTU_RESULT_DECL(dftu_result_series, dftu_series*);
DFTU_RESULT_DECL(dftu_result_lazyframe, dftu_lazyframe*);
DFTU_RESULT_DECL(dftu_result_u64, uint64_t);

/** A byte buffer crossing the ABI. `free_fn` is NULL when the bytes need no
   release (a borrowed view, or storage the producer keeps); otherwise the
   receiver calls free_fn(data, ud) exactly once when it is done with them. */
typedef struct dftu_bytes {
    const void* data;
    uint64_t len;
    void (*free_fn)(void* data, void* ud);
    void* ud;
} dftu_bytes;

/** Work function for spawn()/then(); arg is plugin-owned. */
typedef void (*dftu_work_fn)(void* arg);

/** Per-item sink; `item` points at the producer's item struct, borrowed for the
   call only. `ud` is plugin-owned. */
typedef void (*dftu_stream_item_fn)(const void* item, void* ud);

/** Severity for dftu_plugin_host::log; higher is more severe. Mirrors the
   host's own logger levels, so a plugin's line is gated by the same threshold.
 */
typedef enum {
    DFTU_LOG_TRACE = 0,
    DFTU_LOG_DEBUG = 1,
    DFTU_LOG_INFO = 2,
    DFTU_LOG_WARN = 3,
    DFTU_LOG_ERROR = 4
} dftu_log_level;

/** Services the host lends the plugin; all calls are safe from any slice
 * thread.
 */
struct dftu_plugin_host {
    uint32_t abi_version;
    void* h;

    /** Fetch an optional host-service group by id (DFTU_SVC_*); NULL if absent.
     */
    const void* (*get_service)(void* h, const char* ext_id);

    const char* (*resolve)(void* h, dftu_str id, uint32_t* out_len);
    dftu_str (*intern)(void* h, const char* s, uint32_t len);

    /** Emit a diagnostic line at `level` (a dftu_log_level); `s`/`n` need no
     * NUL.
     */
    void (*log)(void* h, uint8_t level, const char* s, uint32_t n);
};

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_CORE_H */
