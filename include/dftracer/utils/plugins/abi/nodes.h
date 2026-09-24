#ifndef DFTRACER_UTILS_PLUGINS_ABI_NODES_H
#define DFTRACER_UTILS_PLUGINS_ABI_NODES_H

/** @file
 * dftu.svc.nodes: lets a plugin register a named plan node (dftu_node_vt,
 * dftracer/utils/dataframe/abi.h) that dftu_lazyframe_op stacks on a plan.
 * Registration is a build-phase concern, like dftu_svc_providers: call
 * register_node from the plugin factory. The host records the name and
 * unregisters it before the plugin is unloaded, so a plan can never reach a
 * node whose code is gone. Include dftracer/utils/plugins/abi.h rather than
 * this file directly.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_NODES "dftu.svc.nodes@0"

/** Build-phase registry of named plan nodes. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_NODES); the run-time host answers
   it but refuses register_node, as it does register_provider. */
typedef struct dftu_svc_nodes {
    /** Register `vt`/`self` under `name`, which must be `<plugin>.<name>`.
       `vt` is copied; `self` must outlive every cursor the host opens against
       it, and the host calls vt->destroy(self) once the node is
       unregistered. Returns 0 on success, non-zero if `name`/`vt` is NULL,
       `vt->output_schema` or `vt->open` is NULL, `name` fails the namespace
       rule, or the name is already registered. */
    int (*register_node)(void* h, const char* name, const dftu_node_vt* vt,
                         void* self);
} dftu_svc_nodes;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_NODES_H */
