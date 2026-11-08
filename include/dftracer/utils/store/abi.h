// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_ABI_H
#define DFTRACER_UTILS_STORE_ABI_H

#include <dftracer/utils/common/abi.h>
#include <dftracer/utils/common/export.h>
#include <stdint.h>

/*
 * Stable C ABI for the key-value store; mirrors dftracer::utils::store::Store
 * and the types around it (see store/store.h). No C++ type or exception
 * crosses this boundary: a failure comes back as a dftu_error in the result.
 *
 * Blocking: each call that wraps a coroutine (commit, publish, flush, close,
 * pack) runs it to the end on the default runtime and returns when it is done.
 * It is safe to call from a thread of that runtime: the call hands the
 * worker's run slot to the pool for the wait, as dftracer::utils::Runtime
 * block_on does. Every other call returns without waiting for the runtime.
 *
 * Errors: the message of a dftu_error is BORROWED. It stays valid until the
 * next dftu_store_* call on the same thread; copy it if it must live longer.
 * The one exception is dftu_store_cursor_error, whose message lives as long as
 * its cursor.
 *
 * Ownership: a handle a function returns is owned by the caller, who frees it
 * with the matching *_free call. Every *_free call takes NULL. A handle stays
 * valid after the store handle it came from is freed, because it keeps the
 * store state alive. A handle is not thread safe, except a store handle, which
 * is. Bytes a caller passes in are copied or finished with before the call
 * returns, except where a function says otherwise. A pointer may be NULL only
 * where its size or count is 0, or where a function says so.
 *
 * Fork: a store handle that a fork child inherited makes every call fail with
 * DFTU_COND_FORKED_HANDLE. The child calls dftu_store_open for a handle of its
 * own.
 *
 * C++ only: the codec of a namespace, put_ref, Store::erase, the multi-scope
 * and whole-store scans, namespaces(), scopes(), close_all and adopt.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dftu_store_s dftu_store_t;
typedef struct dftu_store_ns_s dftu_store_ns_t;
typedef struct dftu_store_batch_s dftu_store_batch_t;
typedef struct dftu_store_snapshot_s dftu_store_snapshot_t;
typedef struct dftu_store_cursor_s dftu_store_cursor_t;
typedef struct dftu_store_value_s dftu_store_value_t;
typedef struct dftu_store_value_list_s dftu_store_value_list_t;

/** Matches dftracer::utils::store::FoldKind. Append only. */
typedef enum {
    DFTU_STORE_FOLD_NONE = 0,
    DFTU_STORE_FOLD_SUM_I64 = 1,
    DFTU_STORE_FOLD_SUM_F64 = 2,
    DFTU_STORE_FOLD_MIN_I64 = 3,
    DFTU_STORE_FOLD_MAX_I64 = 4,
    DFTU_STORE_FOLD_MIN_F64 = 5,
    DFTU_STORE_FOLD_MAX_F64 = 6,
    DFTU_STORE_FOLD_COUNT = 7,
    DFTU_STORE_FOLD_LAST = 8,
    DFTU_STORE_FOLD_CONCAT = 9
} dftu_store_fold_kind;

/** Matches dftracer::utils::store::ScopeKind. */
typedef enum {
    DFTU_STORE_SCOPE_ROOT = 0, /**< no paths */
    DFTU_STORE_SCOPE_FILE = 1, /**< exactly one path */
    DFTU_STORE_SCOPE_FILES = 2 /**< one or more paths; sorted, deduplicated */
} dftu_store_scope_kind;

/** Matches dftracer::utils::store::ScopeState. */
typedef enum {
    DFTU_STORE_STATE_ABSENT = 0,
    DFTU_STORE_STATE_READY = 1,
    DFTU_STORE_STATE_FAILED = 2,
    DFTU_STORE_STATE_BAD = 3
} dftu_store_scope_state;

/** The change of one dftu_store_publish_target. */
typedef enum {
    DFTU_STORE_PUBLISH_READY = 0,  /**< the building copy becomes current */
    DFTU_STORE_PUBLISH_FAILED = 1, /**< record a Failed copy */
    DFTU_STORE_PUBLISH_DROPPED = 2 /**< hide the whole scope */
} dftu_store_publish_change;

/** A scope: a kind and its paths. `paths` and each path are read during the
 * call only. */
typedef struct dftu_store_scope {
    int32_t kind; /**< a dftu_store_scope_kind value */
    const char* const* paths;
    uint64_t path_count;
} dftu_store_scope;

/** A byte range. In a result it is BORROWED from the object the function names
 * and stays valid as that function says. */
typedef struct dftu_store_bytes {
    const uint8_t* data;
    uint64_t size;
} dftu_store_bytes;

/** The signature of one trace file of a scope. */
typedef struct dftu_store_source_stat {
    uint64_t mtime;
    uint64_t size;
    uint64_t hash;
} dftu_store_source_stat;

/** One state change for dftu_store_publish. `ns` and `scope` are read during
 * the call only. `sources` is NULL, or `source_count` entries, one per path of
 * the scope in its path order, and is used by READY only. */
typedef struct dftu_store_publish_target {
    const dftu_store_ns_t* ns;
    const dftu_store_scope* scope;
    uint64_t params_hash;
    const dftu_store_source_stat* sources;
    uint64_t source_count;
    int32_t change; /**< a dftu_store_publish_change value */
    /** Nonzero with READY in a shared namespace: the new copy stands for the
     * whole scope, and the same record hides every older copy. */
    int32_t replaces_older;
} dftu_store_publish_target;

/** Fill with dftu_store_pack_options_init before setting fields. */
typedef struct dftu_store_pack_options {
    uint64_t small_file_bytes; /**< closed files below this merge into one */
    double dead_ratio;         /**< rewrite a file past this dead share */
    uint64_t keep_seconds;     /**< age before a replaced file is deleted */
} dftu_store_pack_options;

typedef struct dftu_store_pack_report {
    uint64_t files_packed;
    uint64_t copies_written;
    uint64_t copies_skipped;
    uint64_t files_deleted;
    uint64_t bytes_deleted;
} dftu_store_pack_report;

/** The current copy of one scope, as dftu_store_snapshot_copy_info reports it.
 * `source_count` is the number of sources the copy has, which may exceed the
 * capacity the caller gave. */
typedef struct dftu_store_copy {
    int32_t state; /**< a dftu_store_scope_state value */
    uint64_t generation;
    uint64_t params_hash;
    uint64_t source_count;
} dftu_store_copy;

/** `value` is 0 and carries nothing when a call has no result of its own. */
DFTU_RESULT_DECL(dftu_store_void_result, int32_t);
DFTU_RESULT_DECL(dftu_store_open_result, dftu_store_t*);
DFTU_RESULT_DECL(dftu_store_ns_result, dftu_store_ns_t*);
DFTU_RESULT_DECL(dftu_store_batch_result, dftu_store_batch_t*);
DFTU_RESULT_DECL(dftu_store_snapshot_result, dftu_store_snapshot_t*);
DFTU_RESULT_DECL(dftu_store_cursor_result, dftu_store_cursor_t*);
DFTU_RESULT_DECL(dftu_store_value_result, dftu_store_value_t*);
DFTU_RESULT_DECL(dftu_store_value_list_result, dftu_store_value_list_t*);
DFTU_RESULT_DECL(dftu_store_bytes_result, dftu_store_bytes);
DFTU_RESULT_DECL(dftu_store_pack_result, dftu_store_pack_report);
DFTU_RESULT_DECL(dftu_store_copy_result, dftu_store_copy);

/** Open the store in directory `dir`, creating it when it does not exist.
 * Every handle to one directory in this process shares its state. On success
 * the caller owns the handle; free it with dftu_store_free. A NULL `dir` fails
 * with DFTU_COND_INVALID_ARGUMENT. Freeing the last handle to a directory
 * closes its writer but drops a failure: call dftu_store_close first to learn
 * of one. The first call also applies the memory settings from the
 * environment, as the command line tools do at start. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_open_result
dftu_store_open(const char* dir);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_store_free(dftu_store_t* store);

/** Register a namespace with this process's writer. The same name and
 * settings twice return an equal handle; the same name with other settings
 * fails with DFTU_COND_INVALID_ARGUMENT. `name` must not be empty. `fold` is a
 * dftu_store_fold_kind value; `durable` and `shared` are bools. In a shared
 * namespace each scope holds the keys of every writer; it needs
 * DFTU_STORE_FOLD_NONE and refuses erases. On success the caller owns the
 * handle; free it with dftu_store_ns_free. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_ns_result
dftu_store_ns(dftu_store_t* store, const char* name, uint32_t format_version,
              int32_t fold, int32_t durable, int32_t shared);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_store_ns_free(dftu_store_ns_t* ns);

/** Start a batch of puts, erases and folds for `scope` of `ns`. `sources` is
 * NULL or `source_count` entries, one per path of the scope in its path order.
 * The batch copies what it is given. A write error of the batch, such as a put
 * into a fold namespace, is reported by dftu_store_batch_commit. On success
 * the caller owns the batch; commit it with dftu_store_batch_commit, which
 * consumes it, or free it with dftu_store_batch_free to discard it. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_batch_result
dftu_store_batch(dftu_store_t* store, const dftu_store_ns_t* ns,
                 const dftu_store_scope* scope, uint64_t params_hash,
                 const dftu_store_source_stat* sources, uint64_t source_count);

/** A batch of a new copy of `scope` that no reader sees until
 * dftu_store_publish. Ownership as dftu_store_batch. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_batch_result
dftu_store_building_batch(dftu_store_t* store, const dftu_store_ns_t* ns,
                          const dftu_store_scope* scope);

/** Add a key and value. The bytes are copied. A fold namespace rejects it at
 * commit. A NULL `batch`, or a NULL pointer with a size above 0, fails with
 * DFTU_COND_INVALID_ARGUMENT. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result dftu_store_batch_put(
    dftu_store_batch_t* batch, const uint8_t* key, uint64_t key_size,
    const uint8_t* value, uint64_t value_size);

/** Add a fold input to `key`. A numeric fold needs an 8 byte value; a
 * namespace without a fold rejects it at commit. Arguments as
 * dftu_store_batch_put. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result dftu_store_batch_fold(
    dftu_store_batch_t* batch, const uint8_t* key, uint64_t key_size,
    const uint8_t* value, uint64_t value_size);

/** Hide `key` in the copy of this batch's scope. The erases of a batch apply
 * before its puts. Commit reports DFTU_COND_UNSUPPORTED when another data file
 * holds the current copy. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_batch_erase(dftu_store_batch_t* batch, const uint8_t* key,
                       uint64_t key_size);

/** Hide every key in [begin, end). `end_size` 0 means to the last key. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_batch_erase_range(dftu_store_batch_t* batch, const uint8_t* begin,
                             uint64_t begin_size, const uint8_t* end,
                             uint64_t end_size);

/** Commit and consume the batch: the handle is freed whether the call
 * succeeds or fails, so do not use or free it afterward. Waits for writer
 * space when the writer is busy. A reader sees the batch after
 * dftu_store_flush, dftu_store_publish or dftu_store_close, or once its extent
 * fills. A NULL batch fails with DFTU_COND_INVALID_ARGUMENT. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_batch_commit(dftu_store_batch_t* batch);

/** Discard a batch that was not committed. NULL is a no-op. */
DFTU_EXPORT void dftu_store_batch_free(dftu_store_batch_t* batch);

/** Flush, then apply every change of `targets` in one record: a reader sees
 * all of them or none. A bad target changes nothing. `n` 0 only flushes. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result dftu_store_publish(
    dftu_store_t* store, const dftu_store_publish_target* targets, uint64_t n);

/** Record a Failed copy of `scope` with `params_hash`. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_fail(dftu_store_t* store, const dftu_store_ns_t* ns,
                const dftu_store_scope* scope, uint64_t params_hash);

/** Hide the whole scope. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_drop(dftu_store_t* store, const dftu_store_ns_t* ns,
                const dftu_store_scope* scope);

/** Make the committed batches visible to readers. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_flush(dftu_store_t* store);

/** Look for new data of other processes now. dftu_store_snapshot looks at most
 * once per second, so a reader that must see what another process just flushed
 * calls this first. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_refresh(dftu_store_t* store);

/** Flush and close this process's writer. A later write opens a new one. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_void_result
dftu_store_close(dftu_store_t* store);

/** The defaults of @ref dftracer::utils::store::PackOptions "PackOptions". */
DFTU_EXPORT void dftu_store_pack_options_init(dftu_store_pack_options* out);

/** Close this process's writer, rewrite the current copies of the chosen files
 * into one new file and delete the files `options` lets go; see
 * dftracer::utils::store::Store::pack. Run it with no other writer active in
 * the directory. `options` NULL = defaults; it is read during the call. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_pack_result
dftu_store_pack(dftu_store_t* store, const dftu_store_pack_options* options);

/** A point-in-time view of every data file of the store. On success the
 * caller owns it; free it with dftu_store_snapshot_free. Values and cursors
 * taken from it keep it alive. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_snapshot_result
dftu_store_snapshot(dftu_store_t* store);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_store_snapshot_free(dftu_store_snapshot_t* snapshot);

/** The current copy of `scope`. Fills `out`. When `sources` is not NULL, up to
 * `capacity` source signatures are copied to it; `out->source_count` is the
 * full count, so call with `sources` NULL and `capacity` 0 to size an array. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_copy_result
dftu_store_snapshot_copy_info(const dftu_store_snapshot_t* snapshot,
                              const dftu_store_ns_t* ns,
                              const dftu_store_scope* scope,
                              dftu_store_source_stat* sources,
                              uint64_t capacity);

/** The value of `key`. A missing key succeeds with a NULL value. Otherwise the
 * caller owns the value; free it with dftu_store_value_free. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_value_result
dftu_store_snapshot_get(const dftu_store_snapshot_t* snapshot,
                        const dftu_store_ns_t* ns,
                        const dftu_store_scope* scope, const uint8_t* key,
                        uint64_t key_size);

/** One result per key in `n` keys, in order. `keys` is read during the call.
 * The caller owns the list; free it with dftu_store_value_list_free. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_value_list_result
dftu_store_snapshot_multi_get(const dftu_store_snapshot_t* snapshot,
                              const dftu_store_ns_t* ns,
                              const dftu_store_scope* scope,
                              const dftu_store_bytes* keys, uint64_t n);

/** The bytes of a value, valid until the value is freed. A NULL value gives
 * {NULL, 0}. */
DFTU_EXPORT dftu_store_bytes
dftu_store_value_bytes(const dftu_store_value_t* v);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_store_value_free(dftu_store_value_t* v);

/** The number of results of a list; 0 for NULL. */
DFTU_EXPORT uint64_t
dftu_store_value_list_count(const dftu_store_value_list_t* list);

/** Result `i` of a list. Returns 1 and fills `out` with bytes valid until the
 * list is freed, 0 when the key was missing, and -1 for a NULL argument or an
 * `i` past the count. */
DFTU_EXPORT int32_t dftu_store_value_list_at(
    const dftu_store_value_list_t* list, uint64_t i, dftu_store_bytes* out);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_store_value_list_free(dftu_store_value_list_t* list);

/** A scan of the keys that start with `prefix`, in key order. The cursor
 * starts before the first entry: call dftu_store_cursor_next to reach it. The
 * caller owns the cursor; free it with dftu_store_cursor_free. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_cursor_result
dftu_store_snapshot_scan_prefix(const dftu_store_snapshot_t* snapshot,
                                const dftu_store_ns_t* ns,
                                const dftu_store_scope* scope,
                                const uint8_t* prefix, uint64_t prefix_size);

/** A scan of the keys in [begin, end). `end_size` 0 means to the last key.
 * Ownership as dftu_store_snapshot_scan_prefix. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_cursor_result
dftu_store_snapshot_scan_range(const dftu_store_snapshot_t* snapshot,
                               const dftu_store_ns_t* ns,
                               const dftu_store_scope* scope,
                               const uint8_t* begin, uint64_t begin_size,
                               const uint8_t* end, uint64_t end_size);

/** Move to the next entry. Returns 1 when the cursor is on an entry, 0 at the
 * end (check dftu_store_cursor_error to tell the end from a failure) and -1 for
 * a NULL cursor. A failure while moving ends the cursor and is reported by
 * dftu_store_cursor_error. */
DFTU_EXPORT int32_t dftu_store_cursor_next(dftu_store_cursor_t* cursor);

/** The key of the current entry, valid until the next dftu_store_cursor_next
 * or dftu_store_cursor_free. {NULL, 0} when the cursor is NULL, not yet moved
 * or at its end. */
DFTU_EXPORT dftu_store_bytes
dftu_store_cursor_key(const dftu_store_cursor_t* cursor);

/** The value of the current entry, valid as the key is. A cursor that is NULL,
 * not yet moved or at its end, or a block that cannot be read, fails. */
DFTU_EXPORT DFTU_RESULT_MUST_CHECK dftu_store_bytes_result
dftu_store_cursor_value(dftu_store_cursor_t* cursor);

/** Returns 1 and fills `out` when the cursor stopped on a failure, else
 * 0. The message in `out` lives as long as the cursor. */
DFTU_EXPORT int32_t dftu_store_cursor_error(const dftu_store_cursor_t* cursor,
                                            dftu_error* out);

/** NULL is a no-op. */
DFTU_EXPORT void dftu_store_cursor_free(dftu_store_cursor_t* cursor);

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_STORE_ABI_H
