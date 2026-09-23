#ifndef DFTRACER_UTILS_PLUGINS_ABI_PROVIDERS_H
#define DFTRACER_UTILS_PLUGINS_ABI_PROVIDERS_H

/** @file
 * dftu.svc.providers: lets a plugin register itself as a named source, so it
 * can back a LazyFrame the way a file or another engine does. Registration is
 * a build-phase concern: call register_provider from the plugin factory, the
 * same phase dftu_svc_ops::register_op and dftu_svc_agg::register_state run
 * in. Include dftracer/utils/plugins/abi.h rather than this file directly.
 *
 * A registered dftu_source_vt (dftracer/utils/dataframe/abi.h) is a C mirror
 * of the C++ dftracer::utils::dataframe::Source / Cursor pair (lazyframe.h):
 * schema() without a scan, scan() opens a Cursor honoring a dftu_scan_request
 * (projection/filters/limit pushdown), and the Cursor pulls morsels one
 * dftu_dataframe at a time. A provider that ignores the request and reports
 * DFTU_PUSHED_NO for every filter is always sound, since the host re-applies
 * whatever a source did not translate; only projection is not advisory - a
 * provider accepting a non-empty projection must return exactly those
 * columns, in that order, or the host surfaces a scan error.
 *
 * register_provider forwards, after the plugin-name gate, into the
 * process-global provider registry (dftu_provider_register,
 * dftracer/utils/dataframe/abi.h) that dftu_lazyframe_from_provider reads -
 * the registry is not plugin-scoped, since a non-plugin C caller or a future
 * language binding must be able to register and use a provider too.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_PROVIDERS "dftu.svc.providers@0"

/** Build-phase registry of named sources. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_PROVIDERS); the run-time host
   (inside a fold callback) still answers this group so a plugin sees a
   consistent service surface, but refuses register_provider there - by the
   time a scan runs, the set of providers a plan can name is already
   settled. */
typedef struct dftu_svc_providers {
    /** Register `vt`/`self` under `name`. `name` must be `<plugin>.<name>`
       (the bare and "dftu." namespaces are host-reserved, the same rule
       dftu_svc_ops::register_op enforces); `vt` is copied, so it need not
       outlive the call, but `self` must outlive every scan the host later
       opens against it. Returns 0 on success, non-zero if `name`/`vt` is
       NULL, `name` fails the namespace rule, or the name is already
       registered - a second registration under the same name is refused,
       never a silent replace. */
    int (*register_provider)(void* h, const char* name,
                             const dftu_source_vt* vt, void* self);
} dftu_svc_providers;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_PROVIDERS_H */
