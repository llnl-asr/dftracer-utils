// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_INDEX_H
#define DFTRACER_UTILS_PLUGINS_ABI_INDEX_H

/** @file
 * dftu.svc.index: lets a plugin add an index extension. During an index
 * build the host hands the extension's builder every record line of a file,
 * chunk by chunk, or the typed columns of the paths it names, one column per
 * path and chunk, and stores the bytes it returns per chunk and per file.
 * When a query runs, the host asks the extension, per filter leaf, which
 * chunks' bytes may hold a match, and reads only those chunks.
 *
 * Registration is a build-phase concern: call register_extension from the
 * plugin factory. The extension is used by every index build and query in the
 * process while the plugin set that registered it is loaded; the host removes
 * it before the plugin is unloaded. An index built with the extension stays
 * valid without it: data of an extension that is not registered, or whose
 * version or params hash differs, prunes nothing. Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 *
 * Soundness is the plugin's contract: may_match and file_may_match return 0
 * only when the bytes prove that no record they describe satisfies the leaf.
 * A wrong 0 drops matching rows, and the host cannot detect it.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/value.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_INDEX "dftu.svc.index"

/** The operator of a filter leaf. */
typedef int32_t dftu_index_op;
enum {
    DFTU_INDEX_EQ = 0,
    DFTU_INDEX_NE = 1,
    DFTU_INDEX_LT = 2,
    DFTU_INDEX_LE = 3,
    DFTU_INDEX_GT = 4,
    DFTU_INDEX_GE = 5,
    DFTU_INDEX_IN = 6,
    DFTU_INDEX_NOT_IN = 7,
    DFTU_INDEX_LIKE = 8,
    DFTU_INDEX_ILIKE = 9,
    DFTU_INDEX_REGEX = 10,
    DFTU_INDEX_IREGEX = 11,
    DFTU_INDEX_ICONTAINS = 12
};

/** One filter leaf: `path op value`. Borrowed for the compile call only. */
typedef struct dftu_index_leaf {
    const char* path; /**< the dotted JSON path, `path_len` bytes, NUL-ended */
    uint32_t path_len;
    int32_t any;      /**< nonzero for `any(path)`: any element of the array */
    int32_t op;       /**< a dftu_index_op */
    /** A scalar (BOOL, I64, F64 or STR); an ARRAY of scalars for IN and
       NOT_IN; the pattern as STR for LIKE through ICONTAINS. */
    const dftu_value* value;
    /** The dtype code (a `DFTU_TYPE_*` of dftracer/utils/dataframe/c_api.h) of
       the column the leaf targets as the host sees it in the file's catalog,
       or 0 (`DFTU_TYPE_UNKNOWN`) when the host does not know it: the path is
       absent from the catalog, or its values have several types. A plugin that
       stored a hash of the column's values probes with the type it hashed. */
    int32_t type;
} dftu_index_leaf;

/** Host-owned output bytes. append copies `len` bytes of `data`; it may be
   called any number of times during one finish call and not after it. */
typedef struct dftu_index_out {
    uint32_t struct_size; /**< sizeof(dftu_index_out) */
    void* self;
    void (*append)(void* self, const void* data, uint64_t len);
} dftu_index_out;

/** An index extension. Every callback may run on any worker thread; a builder
   is used by one thread at a time, and `self` and compiled leaves may be used
   by several threads at once. Each callback that can fail takes the host's
   @ref dftu_ctx first and returns a @ref dftu_status. */
typedef struct dftu_index_extension {
    uint32_t struct_size; /**< sizeof(dftu_index_extension) */
    /** Data built with another version or params hash is not used, and the
       next build rebuilds it. params_hash identifies the configuration that
       shapes the bytes. */
    uint32_t version;
    uint64_t params_hash;
    /** The largest payload stored; a larger one is dropped, so that chunk or
       file cannot be pruned. 0 means 1 MiB. */
    uint64_t max_payload;

    /** A builder for one file into `*out_builder`; on a negative status
       nothing is stored for the file. */
    dftu_status (*make_builder)(dftu_ctx* ctx, void* self, void** out_builder);
    /** The next `n` record lines of the current chunk, in file order; each
       line is one JSON value without its newline. The lines are borrowed for
       the call (free_fn is NULL). A negative status stores nothing for the
       current chunk; the host logs it once with the ctx message. May be NULL
       when `columns` and `step_column` are set; the host then reads no lines
       for this extension. */
    dftu_status (*step)(dftu_ctx* ctx, void* builder, const dftu_bytes* lines,
                        uint64_t n);
    /** Ends chunk `granule`: append its payload to `out`. No bytes stores
       nothing for the chunk. A negative status stores nothing. */
    dftu_status (*finish_granule)(dftu_ctx* ctx, void* builder,
                                  uint64_t granule, const dftu_index_out* out);
    /** Ends the file: append the file payload to `out`, as finish_granule. */
    dftu_status (*finish_file)(dftu_ctx* ctx, void* builder,
                               const dftu_index_out* out);
    /** Frees a builder; called once for each non-NULL make_builder result. */
    void (*destroy_builder)(void* builder);

    /** Prepare `leaf` for many payloads into `*out`; NULL with DFTU_OK means
       this extension has no evidence for it. */
    dftu_status (*compile)(dftu_ctx* ctx, void* self,
                           const dftu_index_leaf* leaf, void** out);
    /** 0 when chunk payload `p` (`n` bytes, borrowed) proves no record in the
       chunk satisfies the leaf; nonzero otherwise. */
    int (*may_match)(void* compiled, const uint8_t* p, uint64_t n);
    /** As may_match, for the file payload. May be NULL. */
    int (*file_may_match)(void* compiled, const uint8_t* p, uint64_t n);
    /** Frees a compile result; called once for each non-NULL one. */
    void (*release)(void* compiled);

    /** The paths whose columns the extension takes: a NULL-ended array of
       NUL-terminated strings, owned by the plugin and valid until its destroy
       runs; `{"*", NULL}` means every path. A path is the dotted name a filter
       uses: `name`, `cat`, `pid`, `tid`, `ts`, `dur`, `fhash`, `hhash` or
       `args.<key>`. NULL means no column. Needs `step_column`. */
    const char* const* columns;
    /** One column of the current chunk, once per requested path and chunk and
       before `finish_granule`. `name` is the path, `name_len` bytes,
       NUL-ended. `column` has one row per record of the chunk, null where the
       record has no value for the path, and is borrowed: it is valid for the
       call only, so a plugin that keeps data copies it, and it must not free
       it. Its dtype is one of `DFTU_TYPE_INT64`, `DFTU_TYPE_UINT64`,
       `DFTU_TYPE_FLOAT64` or `DFTU_TYPE_STRING`. When the values of a path in
       the chunk have no single type, the host calls nothing for that path and
       stores nothing for the chunk, so the chunk is never pruned. A path the
       chunk lacks entirely is not called. A negative status stores nothing for
       the chunk, as for `step`. May be NULL when `columns` is NULL. */
    dftu_status (*step_column)(dftu_ctx* ctx, void* builder, const char* name,
                               uint32_t name_len, const dftu_series* column);
} dftu_index_extension;

/** Build-phase registry of index extensions. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_INDEX); the run-time host answers it
   but refuses register_extension. */
typedef struct dftu_svc_index {
    uint32_t struct_size; /**< sizeof(dftu_svc_index) */
    /** Register `vt`/`self` under `name`, which must be `<plugin>.<name>`.
       `vt` is copied; `self` stays owned by the plugin and must stay valid
       until the plugin's destroy runs, which the host calls only after
       removing the extension. `make_builder`, `finish_granule`,
       `destroy_builder`, `compile`, `may_match` and `release` are required,
       and so is at least one of `step` and `step_column` with `columns` (both
       may be set, and the host calls each). Fails with DFTU_E_INVALID if
       `name`/`vt` is NULL, a required callback is NULL, the extension has
       neither `step` nor `step_column` with `columns`, `name` fails the
       namespace rule, or the name is already registered; the message names
       the extension. */
    dftu_status (*register_extension)(dftu_ctx* ctx, void* h, const char* name,
                                      const dftu_index_extension* vt,
                                      void* self);

    /** The version of the value hash that `hash_value` and `hash_series`
       compute. A plugin that stores hashes stores this number with them, and
       its `may_match` returns nonzero (keeps the chunk) for bytes of another
       version. */
    uint32_t hash_version;
    /** The host's canonical hash of one value, the hash its own bloom filters
       and posting lists use: every integer hashes as an int64, a double that
       is a whole number in int64 range hashes as that integer (42.0 equals 42),
       every NaN as one value, -0.0 as 0, a string as its bytes, and a string
       never equals a number. BOOL hashes as 0 or 1. A NULL `v`, a NULL value,
       an ARRAY and an OBJECT hash as the null value. `v` is borrowed for the
       call. Pure, and safe from any thread. */
    uint64_t (*hash_value)(const dftu_value* v);
    /** Hashes the `n` rows of `column` into `out[0..n)` with the rules of
       `hash_value`; a null row gets the null value's hash. `column` is
       borrowed for the call and `out` is the caller's, with room for `n`
       entries. Returns DFTU_OK, DFTU_E_INVALID when `column` or `out` is NULL
       or `n` is not the length of `column`, or DFTU_E_UNSUPPORTED for a dtype
       other than the integer, float, string and large string dtypes, or
       DFTU_E_NO_MEMORY when a column that is not flat cannot be gathered; `out`
       is then unchanged. Safe from any thread. */
    dftu_status (*hash_series)(const dftu_series* column, uint64_t* out,
                               uint64_t n);
} dftu_svc_index;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_INDEX_H */
