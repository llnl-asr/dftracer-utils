// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_UDAF_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_UDAF_H

#include <dftracer/utils/dataframe/udaf_adapter.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/types.h>
#include <dftracer/utils/plugins/plugin/udf.h>

namespace dftracer::utils::plugins {

// A plugin UDAF and an in-process one are the same classes and one adapter.
using dataframe::UdafOptions;
using dataframe::UdafStateSet;
using dataframe::UdafType;

namespace detail {
using dataframe::detail::udaf_desc_of;
}  // namespace detail

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_UDAF_H */
