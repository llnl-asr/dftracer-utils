// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_STORE_H
#define DFTRACER_UTILS_PLUGINS_ABI_STORE_H

/** @file
 * dftu.svc.store: the store of the index root the scan reads, lent to a
 * plugin. Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_STORE). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 *
 * Every call forwards one function of the store C ABI
 * (dftracer/utils/store/c_api.h) with the same arguments, the same handles and
 * the same meaning, except that an error goes to the callback's @ref dftu_ctx
 * instead of a dftu_error. The host owns the store: there is no open, close,
 * pack, drop, flush, publish, fail, refresh or building_batch here, and the
 * plugin never sees a directory.
 *
 * Namespaces: the plugin passes a name of its own, `<ns>`, and the host
 * registers it as `plugin.<plugin name>.<ns>`, so a plugin reaches only its
 * own namespaces and two plugins that choose the same `<ns>` keep separate
 * data. A name that starts with `.`, contains `..`, starts with `dftu.` or
 * `server.`, or holds a byte outside [a-z0-9_.-] is refused with
 * DFTU_E_INVALID.
 *
 * Which store: the index root's store of the trace folder the scan reads
 * (trace::index::store_of), the one writer per process of that directory. A
 * scan covers one index root: a unit of a second root makes every call of this
 * group fail with DFTU_E_INVALID and a message naming both roots.
 *
 * Which handles: a namespace, batch or snapshot handle belongs to the store
 * directory the host lent, compared by that directory rather than by the
 * fold's address. Every fold of one index root lends the same directory, so a
 * handle a slice step made works in the same plugin's master finalize; a handle
 * of another root, or a stale one from a fold that ended, is refused with
 * DFTU_E_INVALID rather than reaching another store's namespace of the same
 * id. A NULL handle of any of them, and of a cursor, is refused with
 * DFTU_E_INVALID and a message that names it.
 *
 * Which host: a fold callback's host answers this group. A handle's cursor
 * host and the build host answer NULL, because neither runs over one trace
 * folder. A fold answers it from its first unit on, since the unit's index
 * carries the root; a call before that fails with DFTU_E_INVALID.
 *
 * commit commits the batch, and the store's reader rule applies from then on: a
 * snapshot of this process sees it once its bytes reach a published extent, and
 * a reader in another process sees it after the host's flush at the end of the
 * fold, or earlier once the batch's extent fills. A snapshot can be up to a
 * second old, so read a value back in a later fold rather than in the call that
 * wrote it. The plugin has no flush of its own, since the host holds the one
 * writer of the directory; the host flushes that writer once, from the fold's
 * finalize, whichever fold of the plugin committed, and a failed flush fails
 * the run. Only a durable namespace is synced, so a machine crash can still
 * lose a committed batch of any other namespace. commit consumes its batch
 * whether it commits or is refused. A batch left uncommitted writes nothing.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/store/c_api.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_STORE "dftu.svc.store"

typedef struct dftu_svc_store {
    uint32_t struct_size; /**< sizeof(dftu_svc_store) */

    /** dftu_store_ns of the plugin's `<ns>`. `h` is the callback's void* h. */
    dftu_status (*ns)(dftu_ctx* ctx, void* h, const char* name,
                      uint32_t format_version, dftu_store_fold_kind fold,
                      int32_t durable, int32_t shared, dftu_store_ns_t** out);
    /** dftu_store_ns_free. NULL is a no-op. */
    void (*ns_free)(dftu_store_ns_t* ns);

    /** dftu_store_batch. */
    dftu_status (*batch)(dftu_ctx* ctx, void* h, const dftu_store_ns_t* ns,
                         const dftu_store_scope* scope, uint64_t params_hash,
                         const dftu_store_source_stat* sources,
                         uint64_t source_count, dftu_store_batch_t** out);
    /** dftu_store_batch_put. */
    dftu_status (*put)(dftu_ctx* ctx, dftu_store_batch_t* batch,
                       const uint8_t* key, uint64_t key_size,
                       const uint8_t* value, uint64_t value_size);
    /** dftu_store_batch_fold. */
    dftu_status (*fold)(dftu_ctx* ctx, dftu_store_batch_t* batch,
                        const uint8_t* key, uint64_t key_size,
                        const uint8_t* value, uint64_t value_size);
    /** dftu_store_batch_erase. */
    dftu_status (*erase)(dftu_ctx* ctx, dftu_store_batch_t* batch,
                         const uint8_t* key, uint64_t key_size);
    /** dftu_store_batch_erase_range. */
    dftu_status (*erase_range)(dftu_ctx* ctx, dftu_store_batch_t* batch,
                               const uint8_t* begin, uint64_t begin_size,
                               const uint8_t* end, uint64_t end_size);
    /** dftu_store_batch_commit; consumes `batch`. Durable from the call; the
     *  host flushes its writer once when the fold ends. */
    dftu_status (*commit)(dftu_ctx* ctx, void* h, dftu_store_batch_t* batch);
    /** dftu_store_batch_free. NULL is a no-op. */
    void (*batch_free)(dftu_store_batch_t* batch);

    /** dftu_store_snapshot. `h` is the callback's void* h. */
    dftu_status (*snapshot)(dftu_ctx* ctx, void* h,
                            dftu_store_snapshot_t** out);
    /** dftu_store_snapshot_free. NULL is a no-op. */
    void (*snapshot_free)(dftu_store_snapshot_t* snapshot);

    /** dftu_store_snapshot_get. */
    dftu_status (*get)(dftu_ctx* ctx, const dftu_store_snapshot_t* snapshot,
                       const dftu_store_ns_t* ns, const dftu_store_scope* scope,
                       const uint8_t* key, uint64_t key_size,
                       dftu_store_value_t** out);
    /** dftu_store_snapshot_multi_get. */
    dftu_status (*multi_get)(dftu_ctx* ctx,
                             const dftu_store_snapshot_t* snapshot,
                             const dftu_store_ns_t* ns,
                             const dftu_store_scope* scope,
                             const dftu_store_bytes* keys, uint64_t n,
                             dftu_store_value_list_t** out);
    /** dftu_store_snapshot_scan_prefix. */
    dftu_status (*scan_prefix)(dftu_ctx* ctx,
                               const dftu_store_snapshot_t* snapshot,
                               const dftu_store_ns_t* ns,
                               const dftu_store_scope* scope,
                               const uint8_t* prefix, uint64_t prefix_size,
                               dftu_store_cursor_t** out);
    /** dftu_store_snapshot_scan_range. */
    dftu_status (*scan_range)(dftu_ctx* ctx,
                              const dftu_store_snapshot_t* snapshot,
                              const dftu_store_ns_t* ns,
                              const dftu_store_scope* scope,
                              const uint8_t* begin, uint64_t begin_size,
                              const uint8_t* end, uint64_t end_size,
                              dftu_store_cursor_t** out);
    /** dftu_store_cursor_next: DFTU_END when the cursor moved past the last
     *  entry. */
    dftu_status (*cursor_next)(dftu_ctx* ctx, dftu_store_cursor_t* cursor);
    /** dftu_store_cursor_key. */
    dftu_store_bytes (*cursor_key)(const dftu_store_cursor_t* cursor);
    /** dftu_store_cursor_value. */
    dftu_status (*cursor_value)(dftu_ctx* ctx, dftu_store_cursor_t* cursor,
                                dftu_store_bytes* out);
    /** dftu_store_cursor_free. NULL is a no-op. */
    void (*cursor_free)(dftu_store_cursor_t* cursor);

    /** dftu_store_value_bytes. */
    dftu_store_bytes (*value_bytes)(const dftu_store_value_t* v);
    /** dftu_store_value_free. NULL is a no-op. */
    void (*value_free)(dftu_store_value_t* v);
    /** dftu_store_value_list_count. */
    uint64_t (*value_list_count)(const dftu_store_value_list_t* list);
    /** dftu_store_value_list_at: 1 and a value, 0 for no value at `i`, -1 on a
     *  NULL list or out. */
    int32_t (*value_list_at)(const dftu_store_value_list_t* list, uint64_t i,
                             dftu_store_bytes* out);
    /** dftu_store_value_list_free. NULL is a no-op. */
    void (*value_list_free)(dftu_store_value_list_t* list);
} dftu_svc_store;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_STORE_H */
