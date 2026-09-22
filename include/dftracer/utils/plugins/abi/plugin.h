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
   on_finalize returns NULL when handled synchronously, else a task the host
   awaits. */
typedef struct dftu_plugin {
    uint32_t abi_version;
    void* self; /**< read-only config, shared across slices */

    /** Coarse filter this plugin's own predicate is a subset of, or NULL to
       match all; a DSL string (query::Query::from_string), typically rendered
       from an Expr built with the query builder (F/Field). The host unions
       every loaded plugin's plan_query into the weakest predicate that still
       selects every event any plugin keeps, and pushes that union into the
       shared scan's index prune (Plugins::prune / View::filter) - narrowing
       one plugin's own predicate here would starve the others of events they
       are entitled to, since the scan is shared. */
    const char* (*plan_query)(void* self);
    void* (*make_slice)(void* self);
    /** N rows in scan order = N events, one dftu_dataframe per batch. Must be
       synchronous (return NULL); `df` is owned by the host and valid only for
       the call. */
    dftu_task* (*on_batch)(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host);
    void (*merge)(void* into, void* other);
    dftu_task* (*on_finalize)(void* slice, const dftu_plugin_host* host);
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
       "fhash", an arg as "args.<key>", a virtual field as "resolved.fpath". A
       column that is not listed is absent from the frame, so a lookup for it
       returns NULL.

       Ignored for a plugin that registered states: they are handed the same
       frame, and what they read cannot be seen from the slice that declared
       this, so such a plugin keeps every column. A projection is an
       optimisation and must never cost correctness. */
    const char* const* (*reads)(void* self);
    /** Optional (may be NULL). Bytes `slice` holds: the host's only measure
       of a slice against the scan's memory budget. A plugin that declares no
       size is never asked to reclaim, and its slice grows unbounded. */
    uint64_t (*bytes)(void* slice);
    /** Optional (may be NULL). Free up to `want` bytes of `slice` however
       the plugin can (write part of it out, compact, drop a cache) and
       return how many were freed; 0 says nothing could go. The host calls it
       after an on_batch or merge that left bytes(slice) past the budget,
       and logs once per slice when a call frees nothing while still over,
       so a state that cannot shrink is reported rather than silently
       exceeding the budget. A registered state (dftu_svc_agg) is spilled by
       the host through its serialize pair instead and needs none of this. */
    uint64_t (*reclaim)(void* slice, uint64_t want);
    /** Optional (may be NULL). Rewrite the batch every plugin AFTER this one
       in fold order receives: return a NEW dftu_dataframe (the host frees
       it) with rows dropped or added and columns added, dropped or changed,
       or NULL to pass the batch on unchanged. Called in place of on_batch
       (a transform that also accumulates does so inside the call, on the
       frame it was given). The batch it receives is the whole one, `reads`
       is ignored for a transform; a later plugin still gets its own `reads`
       projection, by name, of the rewritten frame, and its plan_query is
       re-applied to the rewritten rows. Order it with provides / consumes:
       a plugin that must see the rewritten batch consumes a name this one
       provides. Synchronous; the frame is valid only for the call. */
    dftu_dataframe* (*transform)(void* slice, const dftu_dataframe* df,
                                 const dftu_plugin_host* host);
} dftu_plugin;

/** The one symbol the loader resolves via dlsym; the plugin's init. `config` is
   NULL when none was given.

   `h` is a BUILD-PHASE host: its get_service answers only the registration
   groups (DFTU_SVC_OPS, DFTU_SVC_AGG, DFTU_SVC_PORTS, DFTU_SVC_PROVIDERS) and,
   within those, only the registration slots - register_op, register_state,
   port_key and register_provider. Every other group, every non-registration
   slot, and resolve/intern return NULL/failure, and the host fails the load
   naming what was denied. Scanning, spawning, I/O and emitting belong to the
   run-time host the fold callbacks receive. `h` and anything obtained from it
   die when the factory returns; only log() is safe to keep using, and only
   for the duration of the call.

   The returned descriptor is still pure data: plan_query, provides and consumes
   are read after every factory has run, so registering here does not reorder
   the fold or change the prune. */
typedef dftu_plugin* (*dftu_plugin_factory)(dftu_plugin_host* h,
                                            const dftu_value* config);
#define DFTRACER_PLUGIN_FACTORY_SYMBOL "dftracer_plugin"

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_PLUGIN_H */
