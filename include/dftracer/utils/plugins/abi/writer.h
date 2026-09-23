#ifndef DFTRACER_UTILS_PLUGINS_ABI_WRITER_H
#define DFTRACER_UTILS_PLUGINS_ABI_WRITER_H

/** @file
 * dftu.svc.writer: a host-owned parallel output writer lent to a plugin.
 * Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_WRITER). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_WRITER "dftu.svc.writer@1"

typedef struct dftu_svc_writer {
    dftu_task* (*merge_shards)(void* h, const char* target,
                               const char* const* shards, uint32_t n);
    dftu_writer* (*writer_create)(void* h, const char* path,
                                  uint32_t num_workers, int gzip);
    dftu_task* (*writer_open)(void* h, dftu_writer* w);
    dftu_task* (*writer_chunk)(void* h, dftu_writer* w, uint32_t worker,
                               const void* data, uint64_t len);
    dftu_task* (*writer_close)(void* h, dftu_writer* w);
} dftu_svc_writer;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_WRITER_H */
