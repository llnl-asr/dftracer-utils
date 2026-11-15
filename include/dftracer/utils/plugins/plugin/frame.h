// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H

#include <dftracer/utils/dataframe/plugin_frame.h>

/// The frame names of the plugin SDK: @ref dftracer::utils::dataframe::Column
/// "Column", @ref dftracer::utils::dataframe::OwnedDataFrame "OwnedDataFrame"
/// and @ref dftracer::utils::dataframe::FrameBuilder "FrameBuilder" live in the
/// dataframe library, which a plugin links, and are spelled here as this
/// namespace spells them.
namespace dftracer::utils::plugins {

using dataframe::Column;
using dataframe::FrameBuilder;
using dataframe::OwnedDataFrame;

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H
