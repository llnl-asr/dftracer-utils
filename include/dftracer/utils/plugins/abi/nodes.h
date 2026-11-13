// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_NODES_H
#define DFTRACER_UTILS_PLUGINS_ABI_NODES_H

/** @file
 * dftu.svc.nodes: lets a plugin register a named plan node (dftu_node_vt,
 * defined below) that dftu_lazyframe_op stacks on a plan.
 * Registration is a build-phase concern, like dftu_svc_providers: call
 * register_node from the plugin factory. The host records the name and
 * unregisters it before the plugin is unloaded, so no new plan finds it; a
 * plan that already holds the node keeps the plugin library loaded until the
 * plan is gone. Include dftracer/utils/plugins/abi.h rather than
 * this file directly.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_NODES "dftu.svc.nodes"

/** A cursor-shaped plan node a plugin registers under a name, so
 * dftu_lazyframe_op(lf, name, args) becomes a step lower_cursor_chain stacks
 * on the plan like any other op. A streaming node transforms each morsel and
 * passes it on; a breaker node drains its input on the first next() and emits
 * one frame - both are ordinary Cursor implementations on the plugin side, so
 * one protocol covers both shapes. */
typedef struct dftu_node_vt {
    uint32_t struct_size; /**< sizeof(dftu_node_vt) */
    /** Declare the node's output schema given the plan's input schema and
       `args`, without running: append to `out` with the dftu_schema builders,
       the same convention as dftu_source_vt::schema_types. Required (unlike a
       source's optional schema_types) - a node has no separate name-only
       schema() call, so this is the only way the plan learns what columns it
       produces. A column whose type the node cannot determine statically is
       appended with DFTU_TYPE_UNKNOWN rather than guessed. */
    dftu_status (*output_schema)(dftu_ctx* ctx, void* self,
                                 const dftu_schema* in, const dftu_op_arg* args,
                                 dftu_schema* out);
    /** Open an output cursor over an already-open input cursor. Same
       convention as dftu_source_vt::scan: writes *out_cursor_self (it may be
       NULL for a stateless cursor) and *out_vt, which must outlive the
       cursor. `in_cursor_self`/`in_vt` is the upstream stage, already open:
       pull it with in_vt->next(ctx, in_cursor_self, ...), passing the node's
       own ctx, the same way the engine pulls any cursor. On success the node
       takes ownership of the input cursor and must call
       in_vt->destroy(in_cursor_self) exactly once, whenever the node's own
       output cursor is destroyed (or earlier, once fully drained); on failure
       (a negative status) the node must not have taken that ownership - the
       caller still owns and destroys the input cursor. `memory_budget` is the
       plan's memory budget in bytes, already resolved, so never 0: a node
       that holds state sizes its spill threshold and its output frames from
       it. */
    dftu_status (*open)(dftu_ctx* ctx, void* self, void* in_cursor_self,
                        const dftu_cursor_vt* in_vt, const dftu_op_arg* args,
                        uint64_t memory_budget, void** out_cursor_self,
                        const dftu_cursor_vt** out_vt);
    /** Release the node; called once, after every cursor opened against it
       has been destroyed. May be NULL for a stateless node. */
    void (*destroy)(void* self);
} dftu_node_vt;

/** Build-phase registry of named plan nodes. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_NODES); the run-time host answers
   it but refuses register_node, as it does register_provider. */
typedef struct dftu_svc_nodes {
    uint32_t struct_size; /**< sizeof(dftu_svc_nodes) */
    /** Register `vt`/`self` under `name`, which must be `<plugin>.<name>`.
       `vt` is copied; `self` must outlive every cursor the host opens against
       it, and the host calls vt->destroy(self) once the node is
       unregistered and no plan holds it any more. Fails with DFTU_E_INVALID if
       `name` is NULL or empty, `vt` is NULL, `vt->output_schema` or `vt->open`
       is NULL, `name` fails the namespace rule, or the name is already
       registered. */
    dftu_status (*register_node)(dftu_ctx* ctx, void* h, const char* name,
                                 const dftu_node_vt* vt, void* self);
} dftu_svc_nodes;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_NODES_H */
