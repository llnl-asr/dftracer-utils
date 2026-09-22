#ifndef DFTRACER_UTILS_PLUGINS_ABI_H
#define DFTRACER_UTILS_PLUGINS_ABI_H

/** @file
 * Stable C ABI for dftracer-utils plugins; include only this header. Each
 * DFTU_SVC_* service group lives in its own dftracer/utils/plugins/abi/ part
 * header, included below; abi/core.h holds the fundamentals every part
 * shares (value types, opaque handles, the error/result channel, the host
 * descriptor) and abi/plugin.h the plugin descriptor itself.
 */

#include <dftracer/utils/plugins/abi/agg.h>
#include <dftracer/utils/plugins/abi/arrow.h>
#include <dftracer/utils/plugins/abi/compose.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/coro.h>
#include <dftracer/utils/plugins/abi/io.h>
#include <dftracer/utils/plugins/abi/ops.h>
#include <dftracer/utils/plugins/abi/plugin.h>
#include <dftracer/utils/plugins/abi/ports.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <dftracer/utils/plugins/abi/query.h>
#include <dftracer/utils/plugins/abi/result.h>
#include <dftracer/utils/plugins/abi/sketch.h>
#include <dftracer/utils/plugins/abi/trace.h>
#include <dftracer/utils/plugins/abi/value.h>
#include <dftracer/utils/plugins/abi/writer.h>

#endif /* DFTRACER_UTILS_PLUGINS_ABI_H */
