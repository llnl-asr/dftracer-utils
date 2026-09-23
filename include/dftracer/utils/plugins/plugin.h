#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_H

/* Header-only C++ wrapper over abi.h; derive a Slice with step/merge/finalize
   and expose it with make_plugin<Slice>(). */

/* Umbrella over the plugin author SDK sub-headers. Include order is dependency
   order: async and types first, then the Host facade, then the registration
   glue and the plugin-entry macro. Each sub-header is self-contained, so the
   guard below only keeps this file's order readable as the layering. */
// clang-format off
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/types.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/register.h>
// clang-format on

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_H */
