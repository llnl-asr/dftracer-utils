#ifndef DFTRACER_UTILS_PLUGINS_ABI_INDEX_H
#define DFTRACER_UTILS_PLUGINS_ABI_INDEX_H

/** @file
 * dftu.svc.index: lets a plugin add an index extension. During an index
 * build the host hands the extension's builder every record line of a file,
 * chunk by chunk, and stores the bytes it returns per chunk and per file.
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

#define DFTU_SVC_INDEX "dftu.svc.index@0"

/** The operator of a filter leaf. */
typedef enum {
    DFTU_INDEX_EQ = 0,
    DFTU_INDEX_NE,
    DFTU_INDEX_LT,
    DFTU_INDEX_LE,
    DFTU_INDEX_GT,
    DFTU_INDEX_GE,
    DFTU_INDEX_IN,
    DFTU_INDEX_NOT_IN,
    DFTU_INDEX_LIKE,
    DFTU_INDEX_ILIKE,
    DFTU_INDEX_REGEX,
    DFTU_INDEX_IREGEX,
    DFTU_INDEX_ICONTAINS
} dftu_index_op;

/** One filter leaf: `path op value`. Borrowed for the compile call only. */
typedef struct dftu_index_leaf {
    const char* path; /**< the dotted JSON path, `path_len` bytes, NUL-ended */
    uint32_t path_len;
    int32_t any;      /**< nonzero for `any(path)`: any element of the array */
    int32_t op;       /**< a dftu_index_op */
    /** A scalar (BOOL, I64, F64 or STR); an ARRAY of scalars for IN and
       NOT_IN; the pattern as STR for LIKE through ICONTAINS. */
    const dftu_value* value;
} dftu_index_leaf;

/** Host-owned output bytes. append copies `len` bytes of `data`; it may be
   called any number of times during one finish call and not after it. */
typedef struct dftu_index_out {
    void* ctx;
    void (*append)(void* ctx, const void* data, uint64_t len);
} dftu_index_out;

/** An index extension. Every callback may run on any worker thread; a builder
   is used by one thread at a time, and `self` and compiled leaves may be used
   by several threads at once. */
typedef struct dftu_index_extension {
    /** Data built with another version or params hash is not used, and the
       next build rebuilds it. params_hash identifies the configuration that
       shapes the bytes. */
    uint32_t version;
    uint64_t params_hash;
    /** The largest payload stored; a larger one is dropped, so that chunk or
       file cannot be pruned. 0 means 1 MiB. */
    uint64_t max_payload;

    /** A builder for one file, or NULL on failure (nothing is stored for the
       file). */
    void* (*make_builder)(void* self);
    /** The next `n` record lines of the current chunk, in file order; each
       line is one JSON value without its newline. The lines are borrowed for
       the call (free_fn is NULL). Returns 0, or nonzero to store nothing for
       the current chunk. */
    int (*step)(void* builder, const dftu_bytes* lines, uint64_t n);
    /** Ends chunk `granule`: append its payload to `out`. No bytes stores
       nothing for the chunk. Returns 0, or nonzero to store nothing. */
    int (*finish_granule)(void* builder, uint64_t granule,
                          const dftu_index_out* out);
    /** Ends the file: append the file payload to `out`, as finish_granule. */
    int (*finish_file)(void* builder, const dftu_index_out* out);
    /** Frees a builder; called once for each non-NULL make_builder result. */
    void (*destroy_builder)(void* builder);

    /** Prepare `leaf` for many payloads, or NULL when this extension has no
       evidence for it. */
    void* (*compile)(void* self, const dftu_index_leaf* leaf);
    /** 0 when chunk payload `p` (`n` bytes, borrowed) proves no record in the
       chunk satisfies the leaf; nonzero otherwise. */
    int (*may_match)(void* compiled, const uint8_t* p, uint64_t n);
    /** As may_match, for the file payload. May be NULL. */
    int (*file_may_match)(void* compiled, const uint8_t* p, uint64_t n);
    /** Frees a compile result; called once for each non-NULL one. */
    void (*release)(void* compiled);
} dftu_index_extension;

/** Build-phase registry of index extensions. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_INDEX); the run-time host answers it
   but refuses register_extension. */
typedef struct dftu_svc_index {
    /** Register `vt`/`self` under `name`, which must be `<plugin>.<name>`.
       `vt` is copied; `self` stays owned by the plugin and must stay valid
       until the plugin's destroy runs, which the host calls only after
       removing the extension. `make_builder`, `step`, `finish_granule`,
       `destroy_builder`, `compile`, `may_match` and `release` are required.
       Returns 0 on success, non-zero if `name`/`vt` is NULL, a required
       callback is NULL, `name` fails the namespace rule, or the name is
       already registered. */
    int (*register_extension)(void* h, const char* name,
                              const dftu_index_extension* vt, void* self);
} dftu_svc_index;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_INDEX_H */
