// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_UDF_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_UDF_H

#include <dftracer/utils/dataframe/udf_adapter.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/types.h>

namespace dftracer::utils::plugins {

// A plugin UDF and an in-process one are the same classes and one adapter.
using dataframe::UdfBatch;
using dataframe::UdfBind;
using dataframe::UdfOptions;
using dataframe::UdfType;

namespace detail {
using dataframe::detail::udf_desc_of;
}  // namespace detail

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_UDF_H */
