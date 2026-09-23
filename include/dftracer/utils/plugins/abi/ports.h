#ifndef DFTRACER_UTILS_PLUGINS_ABI_PORTS_H
#define DFTRACER_UTILS_PLUGINS_ABI_PORTS_H

/** @file
 * dftu.svc.ports: batch-scoped producer -> consumer channels lent to a
 * plugin. Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_PORTS). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_PORTS "dftu.svc.ports@1"

/** Batch-scoped ports for intra-batch producer -> consumer communication. A
   port is a name; a producer and a consumer are wired by naming the same one,
   and a name that ever needs a version carries it in the name. The bus resets
   between batches. A consume result is borrowed until the current on_batch
   returns (copy to retain) and NULL if the producer has not published this
   batch. The host runs a producer before every consumer of its ports; that
   order comes from the plugins' declared dftu_plugin::provides /
   dftu_plugin::consumes, not from registration order.
 */
typedef struct dftu_svc_ports {
    /** Stable key for the port named `name` (lowercase ASCII [a-z0-9._/-]).
       The "dftu." namespace belongs to the host and is refused to plugins. */
    uint64_t (*port_key)(void* h, const char* name);
    void (*publish)(void* h, uint64_t key, const void* data, uint32_t len);
    const void* (*consume)(void* h, uint64_t key, uint32_t* out_len);
} dftu_svc_ports;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_PORTS_H */
