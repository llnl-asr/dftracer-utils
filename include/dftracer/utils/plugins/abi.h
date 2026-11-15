// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_H
#define DFTRACER_UTILS_PLUGINS_ABI_H

/** @file
 * Stable C ABI for dftracer-utils plugins; include only this header. Each
 * DFTU_SVC_* service group lives in its own dftracer/utils/plugins/abi/ part
 * header, included below; abi/core.h holds the fundamentals every part
 * shares (value types, opaque handles, the host descriptor) and abi/plugin.h
 * the plugin descriptor itself.
 */

#include <dftracer/utils/plugins/abi/agg.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/duql.h>
#include <dftracer/utils/plugins/abi/file_formats.h>
#include <dftracer/utils/plugins/abi/fileio.h>
#include <dftracer/utils/plugins/abi/format.h>
#include <dftracer/utils/plugins/abi/fs.h>
#include <dftracer/utils/plugins/abi/index.h>
#include <dftracer/utils/plugins/abi/io.h>
#include <dftracer/utils/plugins/abi/memory.h>
#include <dftracer/utils/plugins/abi/nodes.h>
#include <dftracer/utils/plugins/abi/ops.h>
#include <dftracer/utils/plugins/abi/plugin.h>
#include <dftracer/utils/plugins/abi/ports.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <dftracer/utils/plugins/abi/result.h>
#include <dftracer/utils/plugins/abi/sketch.h>
#include <dftracer/utils/plugins/abi/store.h>
#include <dftracer/utils/plugins/abi/task.h>
#include <dftracer/utils/plugins/abi/trace.h>
#include <dftracer/utils/plugins/abi/value.h>

#endif /* DFTRACER_UTILS_PLUGINS_ABI_H */
