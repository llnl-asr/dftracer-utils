// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_C_API_H
#define DFTRACER_UTILS_TRACE_INDEX_C_API_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/common/export.h>
#include <stdint.h>

/*
 * Stable C ABI for building and inspecting the index of a set of .pfw.gz
 * traces; mirrors dftracer::utils::trace::index::Indexer. Every call blocks
 * until done and runs on the default runtime. No C++ exception crosses this
 * boundary. A fallible function returns a dftu_status, gives its result
 * through an out-parameter and takes a caller-owned dftu_error as its last
 * parameter; see common/c_api.h.
 *
 * Errors: a failing call fills `err` (which may be NULL) and leaves its other
 * out-parameters unchanged, except that an out pointer is set to NULL. A
 * successful call leaves `err` as it was. The caller releases a filled `err`
 * with dftu_error_release.
 *
 * C++ only (dftracer::utils::trace::index::Indexer): the bloom false-positive
 * rate and auto-field tuning, a caller-chosen runtime, the async forms, and the
 * ready / needs-work path lists of a status.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dftu_indexer dftu_indexer;
typedef struct dftu_indexer_file_list dftu_indexer_file_list;

/** Fill with dftu_indexer_options_init before setting fields. A caller that
 * builds one by hand sets `struct_size` to its sizeof; a library newer than the
 * caller uses the defaults for the fields the caller does not have. */
typedef struct dftu_indexer_options {
    uint32_t struct_size;
    const char* index_dir;    /**< NULL or "" = beside each trace */
    uint64_t checkpoint_size; /**< bytes; 0 = default */
    uint32_t parallelism;     /**< workers; 0 = all cores */
    int32_t checkpoints;      /**< bool: build the checkpoint tier */
    int32_t bloom;            /**< bool: build the bloom tier */
    int32_t bloom_required;   /**< bool: a missing bloom tier needs work */
    const char* const* bloom_fields; /**< args fields indexed by name */
    uint32_t bloom_field_count;
    /** Pruning extensions to build ("zonemap", "bloom", "counts",
     * "postings"); a count of 0 builds all four. */
    const char* const* extensions;
    uint32_t extension_count;
    /** Bytes a build may hold at once; 0 = about a third of available
     * memory, UINT64_MAX = unbounded. */
    uint64_t memory_budget;
    /** A registered schema id; NULL or "" detects each file's schema. */
    const char* schema;
    /** Also index each file's other args paths, most frequent first, while
     * their evidence fits `bloom_stats_share`, and, when this is above 0, at
     * most this many of them; 0 = no count limit. Used when `bloom` is set. */
    uint64_t bloom_path_budget;
    /** The share of a file's compressed size its automatic evidence may use,
     * at least 8 MiB; the default is 0.05. A value outside (0, 1] makes
     * dftu_indexer_open fail with DFTU_E_INVALID and a message
     * naming stats_share. A registered schema's index.stats_share and
     * index.path_budget override both fields. */
    double bloom_stats_share;
    /** The aggregation tier is built with every index; these fields adjust the
     * config it is built with. 0 keeps the standard value. */
    uint64_t aggregation_time_interval_us; /**< window; 0 = the standard 1 s */
    int32_t aggregation_compute_percentiles; /**< 0 = the standard, which
                                                stores them */
    int32_t aggregation_group_by_file;       /**< 0 = the standard, which keys
                                                rows by file */
} dftu_indexer_options;

typedef struct dftu_indexer_report {
    uint64_t total;
    uint64_t ready;
    uint64_t needs_work;
    uint64_t indexed;   /**< files this call indexed; 0 for status */
    int32_t aggregation_needs_rebuild;
    uint64_t truncated; /**< ready files indexed from a cut last member */
} dftu_indexer_report;

typedef struct dftu_indexer_file {
    const char* path;       /**< borrowed from the file list */
    const char* index_path; /**< borrowed from the file list */
    uint64_t size_bytes;    /**< as recorded when indexed */
    uint64_t mtime;         /**< as recorded when indexed */
    int32_t truncated;      /**< bool: last gzip member was cut short */
    const char* schema;     /**< borrowed from the file list */
} dftu_indexer_file;

/** The defaults of @ref dftracer::utils::trace::index::IndexerOptions
 * "IndexerOptions": checkpoints and bloom on and required, and the aggregation
 * tier built with the standard config. Sets `struct_size`. */
DFTU_EXPORT void dftu_indexer_options_init(dftu_indexer_options* out);

/** Open `n` trace files or directories (a directory contributes the trace
 * files, .pfw, .jsonl and .ndjson, plain or gzip, directly inside it).
 * `options` NULL = defaults; it is copied, so it and its strings may be freed
 * after the call. A `struct_size` that does not reach the first field after
 * it fails with DFTU_E_INVALID. On success `*out` is an indexer the caller
 * owns; free it with dftu_indexer_free. Writes nothing. */
DFTU_EXPORT dftu_status dftu_indexer_open(const char* const* paths, uint64_t n,
                                          const dftu_indexer_options* options,
                                          dftu_indexer** out, dftu_error* err);

/** Counts of ready and needing-work files in `*out`. Writes nothing. */
DFTU_EXPORT dftu_status dftu_indexer_status(dftu_indexer* indexer,
                                            dftu_indexer_report* out,
                                            dftu_error* err);

/** Build the requested tiers for files that lack them or changed. */
DFTU_EXPORT dftu_status dftu_indexer_build(dftu_indexer* indexer,
                                           dftu_indexer_report* out,
                                           dftu_error* err);

/** Rebuild the requested tiers for every file. */
DFTU_EXPORT dftu_status dftu_indexer_rebuild(dftu_indexer* indexer,
                                             dftu_indexer_report* out,
                                             dftu_error* err);

/** The manifest of every indexed file as JSON (see
 * dftracer::utils::trace::index::to_json). On success `*out` is a string the
 * caller owns; free it with dftu_indexer_string_free. Writes nothing. */
DFTU_EXPORT dftu_status dftu_indexer_manifest(dftu_indexer* indexer, char** out,
                                              dftu_error* err);

/** Per file, the chunks `query` reads and what each pruning extension rules
 * out alone, as JSON. On success `*out` is a string the caller owns; free it
 * with dftu_indexer_string_free. A NULL query or one that does not parse fails
 * with DFTU_E_INVALID. Writes nothing. */
DFTU_EXPORT dftu_status dftu_indexer_explain(dftu_indexer* indexer,
                                             const char* query, char** out,
                                             dftu_error* err);

/** Rewrite one tier extension ("zonemap", "bloom", "counts", "postings",
 * "dft.stats", "dft.metadata" or a registered plugin extension's name) of
 * every file and index files not yet indexed. Another or a NULL name fails
 * with DFTU_E_INVALID and changes nothing. */
DFTU_EXPORT dftu_status dftu_indexer_rebuild_extension(dftu_indexer* indexer,
                                                       const char* extension,
                                                       dftu_indexer_report* out,
                                                       dftu_error* err);

/** Remove one tier extension from every indexed file; names as for
 * dftu_indexer_rebuild_extension. */
DFTU_EXPORT dftu_status dftu_indexer_drop_extension(dftu_indexer* indexer,
                                                    const char* extension,
                                                    dftu_indexer_report* out,
                                                    dftu_error* err);

/** Register the schema the YAML or JSON spec `text` describes (see
 * dftracer::utils::trace::index::register_schema); `source` names it in errors,
 * NULL = "<text>". Gives its id in `*out`. Registered schemas last for the
 * process; registering the same definition again succeeds, another definition
 * of a registered id fails with DFTU_E_INVALID. On success the caller owns the
 * string; free it with dftu_indexer_string_free. */
DFTU_EXPORT dftu_status dftu_schema_register(const char* text,
                                             const char* source, char** out,
                                             dftu_error* err);

/** Register every spec at `path`: a file, or the .yaml, .yml and .json files
 * of a directory. Gives the registered schemas as dftu_schema_list does;
 * ownership as dftu_schema_register. */
DFTU_EXPORT dftu_status dftu_schema_load(const char* path, char** out,
                                         dftu_error* err);

/** The registered schemas as a JSON array (see
 * dftracer::utils::trace::index::schemas_json); ownership as
 * dftu_schema_register. */
DFTU_EXPORT dftu_status dftu_schema_list(char** out, dftu_error* err);

/** The id of the schema detected for the trace at `path` from its first
 * lines; ownership as dftu_schema_register. */
DFTU_EXPORT dftu_status dftu_schema_detect(const char* path, char** out,
                                           dftu_error* err);

/** Why the trace at `path` gets its schema: a JSON object with the chosen id,
 * the sampled object and record counts and every registered schema's required
 * path count and share (see dftracer::utils::trace::index::to_json); ownership
 * as dftu_schema_register. */
DFTU_EXPORT dftu_status dftu_schema_explain(const char* path, char** out,
                                            dftu_error* err);

/** Free a string from dftu_indexer_manifest, dftu_indexer_explain or a
 * dftu_schema_* call. NULL is a no-op. */
DFTU_EXPORT void dftu_indexer_string_free(char* s);

/** The indexed files. On success `*out` is a list the caller owns; free it with
 * dftu_indexer_file_list_free. The list outlives its indexer. */
DFTU_EXPORT dftu_status dftu_indexer_files(dftu_indexer* indexer,
                                           dftu_indexer_file_list** out,
                                           dftu_error* err);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_indexer_free(dftu_indexer* indexer);

DFTU_EXPORT uint64_t
dftu_indexer_file_list_count(const dftu_indexer_file_list* list);

/** Entry `i` in `*out`; its strings stay valid until the list is freed. Fails
 * with DFTU_E_INVALID when `list` or `out` is NULL or `i` >= the count. */
DFTU_EXPORT dftu_status
dftu_indexer_file_list_get(const dftu_indexer_file_list* list, uint64_t i,
                           dftu_indexer_file* out, dftu_error* err);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_indexer_file_list_free(dftu_indexer_file_list* list);

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_TRACE_INDEX_C_API_H
