// SPDX-License-Identifier: MIT
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
#include <dftracer/utils/plugins/plugin/fs.h>
#include <dftracer/utils/plugins/plugin/job.h>
#include <dftracer/utils/plugins/plugin/register.h>
#include <dftracer/utils/plugins/plugin/frame.h>
#include <dftracer/utils/plugins/plugin/source.h>
#include <dftracer/utils/plugins/plugin/file_format.h>
#include <dftracer/utils/plugins/plugin/node.h>
#include <dftracer/utils/plugins/plugin/fileio.h>
#include <dftracer/utils/plugins/plugin/format.h>
#include <dftracer/utils/plugins/plugin/memory.h>
#include <dftracer/utils/plugins/plugin/store.h>
// clang-format on

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_H */
