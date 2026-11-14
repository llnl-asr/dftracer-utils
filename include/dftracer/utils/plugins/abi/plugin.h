// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_PLUGIN_H
#define DFTRACER_UTILS_PLUGINS_ABI_PLUGIN_H

/** @file
 * The plugin descriptor and the loader's factory symbol. Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/value.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the loader reads before it runs any plugin code: the plugin's
   name, and the ABI version and header hash it was built against. */
typedef struct dftu_plugin_info {
    uint32_t struct_size;
    uint32_t abi_version; /**< DFTRACER_UTILS_PLUGIN_ABI_VERSION */
    uint64_t abi_hash;    /**< DFTRACER_UTILS_PLUGIN_ABI_HASH */
    const char* name;     /**< the plugin's own name; may be NULL */
    const char* version;  /**< the plugin's own version text; may be NULL */
} dftu_plugin_info;

#define DFTRACER_UTILS_PLUGIN_INFO_SYMBOL "dftracer_plugin_info"

/** Any value kind is accepted for a declared config key. */
#define DFTU_CONFIG_ANY (-1)

/** One config key a plugin reads, for validation and for --describe. */
typedef struct dftu_config_key {
    const char* name; /**< key at the top level of the config object; NULL
                          terminates the array */
    int32_t kind;     /**< a dftu_value_kind the value must have, or
                          DFTU_CONFIG_ANY. BOOL/I64/F64 accept each other,
                          since dftu_as_i64/f64/bool coerce between them */
    int32_t required; /**< nonzero: the load fails when the key is absent */
    const char* doc;  /**< one line, no trailing period; may be NULL */
} dftu_config_key;

/** A plugin: a data-parallel fold; one slice per worker, merged then finalized.
   on_finalize leaves `pending` NULL when it finished synchronously, else sets
   a task the host awaits. */
typedef struct dftu_plugin {
    uint32_t struct_size;
    void* self; /**< read-only config, shared across slices */

    /** Coarse filter this plugin's own predicate is a subset of, or NULL to
       match all; a DSL string (duql::Query::from_string), typically rendered
       from an Expr built with the query builder (F/Field). The host unions
       every loaded plugin's plan_duql into the weakest predicate that still
       selects every event any plugin keeps, and pushes that union into the
       shared scan's index prune (Plugins::prune / View::filter) - narrowing
       one plugin's own predicate here would starve the others of events they
       are entitled to, since the scan is shared. */
    const char* (*plan_duql)(void* self);
    dftu_status (*make_slice)(dftu_ctx* ctx, void* self, void** out_slice);
    /** N rows in scan order = N events, one dftu_dataframe per batch.
       Synchronous; `df` is owned by the host and valid only for the call. */
    dftu_status (*on_batch)(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host);
    dftu_status (*merge)(dftu_ctx* ctx, void* into, void* other);
    /** Set *pending to finish later; NULL = finished now. */
    dftu_status (*on_finalize)(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending);
    void (*destroy_slice)(void* slice); /**< one call per make_slice */
    void (*destroy)(void* self);        /**< plugin teardown; frees self */

    /** The names this plugin produces, as a NULL-terminated array that outlives
       the plugin; NULL = none. One namespace covers both edge kinds: a
       dftu_svc_ports port it publishes and a dftu_svc_agg accumulator it
       creates. Two plugins providing the same name is a load error. */
    const char* const* (*provides)(void* self);
    /** The names this plugin reads, as a NULL-terminated array that outlives
       the plugin; NULL = none: ports it consumes and accumulators it fetches
       with dftu_svc_agg::agg_result. The host runs every provider of a
       consumed name first, and rejects the set when no loaded plugin provides
       one or when the resulting graph has a cycle. */
    const char* const* (*consumes)(void* self);
    /** The config keys this plugin reads, as an array terminated by an entry
       with a NULL name that outlives the plugin; NULL = undeclared. Declaring
       them makes the host VALIDATE the config before the plugin runs: a key
       that is not declared, a declared key of the wrong kind, and a missing
       required key each fail the load, so a typo is reported instead of
       silently doing nothing. An undeclared plugin is validated not at all. */
    const dftu_config_key* (*config_keys)(void* self);
    /** The batch COLUMNS this plugin reads, as a NULL-terminated array of
       column names that outlives the plugin; NULL = every column.

       Undeclared, the host materializes the whole batch for this plugin: seven
       fixed columns, fhash/hhash, and one column per distinct arg key in the
       batch. That last part is unbounded - a trace with fifty arg keys builds
       fifty Series per batch even for a plugin that reads `dur`. Declaring
       what it reads is the projection that avoids them.

       Names are batch column names as on_batch sees them: "dur", "cat",
       "fhash", an arg as "args.<key>". A column that is not listed is absent
       from the frame, so a lookup for it returns NULL.

       Ignored for a plugin that registered states: they are handed the same
       frame, and what they read cannot be seen from the slice that declared
       this, so such a plugin keeps every column. A projection is an
       optimisation and must never cost correctness. */
    const char* const* (*reads)(void* self);
    /* A slice that holds growing state reserves its bytes through
       dftu_svc_memory (DFTU_SVC_MEMORY) and sets a reclaimer there. A
       registered state (dftu_svc_agg) is spilled by the host through its
       serialize pair instead. */
    /** Optional (may be NULL). Rewrite the batch every plugin AFTER this one
       in fold order receives: set *out to a NEW dftu_dataframe (the host frees
       it) with rows dropped or added and columns added, dropped or changed,
       or to NULL to pass the batch on unchanged. Called in place of on_batch
       (a transform that also accumulates does so inside the call, on the
       frame it was given). The batch it receives is the whole one, `reads`
       is ignored for a transform; a later plugin still gets its own `reads`
       projection, by name, of the rewritten frame, and its plan_duql is
       re-applied to the rewritten rows. Order it with provides / consumes:
       a plugin that must see the rewritten batch consumes a name this one
       provides. Synchronous; the frame is valid only for the call. */
    dftu_status (*transform)(dftu_ctx* ctx, void* slice,
                             const dftu_dataframe* df,
                             const dftu_plugin_host* host,
                             dftu_dataframe** out);
} dftu_plugin;

/** The plugin's init, resolved with dlsym after dftracer_plugin_info passed.
   `config` is NULL when none was given. On success *out is the descriptor.

   `h` is a BUILD-PHASE host: its get_service answers only the registration
   groups (DFTU_SVC_OPS, DFTU_SVC_AGG, DFTU_SVC_PORTS, DFTU_SVC_PROVIDERS,
   DFTU_SVC_NODES, DFTU_SVC_INDEX, DFTU_SVC_FILE_FORMATS) and, within those,
   only the registration slots - register_op, register_udf, register_udaf,
   register_state, port_key, register_provider, register_node,
   register_extension, register_format and register_sink - plus DFTU_SVC_TASK
   with its run slot, which uses no host state, so a plugin may keep that
   service pointer and its `h` for later. Every other group, every
   non-registration slot, and resolve/intern return NULL/failure, and the host
   fails the load naming what was denied. Scanning, spawning, I/O and emitting
   belong to the run-time host the fold callbacks receive. `h` and anything
   obtained from it die when the factory returns; only log() is safe to keep
   using, and only for the duration of the call.

   The host keeps the plugin library loaded while a node, provider, index
   extension or op registered here, or a plan or cursor holding one, is alive;
   unloading the plugin removes the names at once. The plugin's own destroy
   runs after the last of them is gone, then the library is closed, so a
   node's or provider's destroy may use the plugin's global state.

   The returned descriptor is still pure data: plan_duql, provides and consumes
   are read after every factory has run, so registering here does not reorder
   the fold or change the prune. */
typedef dftu_status (*dftu_plugin_factory)(dftu_ctx* ctx, dftu_plugin_host* h,
                                           const dftu_value* config,
                                           dftu_plugin** out);
#define DFTRACER_UTILS_PLUGIN_FACTORY_SYMBOL "dftracer_plugin"

/**
 * @brief The symbol the host resolves with dlsym; a plugin defines it as its
 * @ref dftu_plugin_factory. Declared here so a plugin's definition has a
 * prior declaration.
 */
DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out);

/**
 * @brief The record the loader checks before it runs the factory; a plugin
 * defines it with @ref DFTU_PLUGIN_INFO. A missing symbol, or an ABI version
 * or header hash that differs from the host's, refuses the plugin.
 */
DFTU_PLUGIN_EXPORT const dftu_plugin_info* dftracer_plugin_info(void);

/** Define dftracer_plugin_info for a plugin named `name` at `version` (string
   literals or NULL). Use once per plugin library. */
#define DFTU_PLUGIN_INFO(name, version)                                     \
    DFTU_PLUGIN_EXPORT const dftu_plugin_info* dftracer_plugin_info(void) { \
        static const dftu_plugin_info info = {                              \
            sizeof(dftu_plugin_info), DFTRACER_UTILS_PLUGIN_ABI_VERSION,    \
            DFTRACER_UTILS_PLUGIN_ABI_HASH, (name), (version)};             \
        return &info;                                                       \
    }

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_PLUGIN_H */
