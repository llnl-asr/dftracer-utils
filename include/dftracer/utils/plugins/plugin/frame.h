// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H

#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>

/// The frame names of the plugin SDK: @ref dftracer::utils::dataframe::Column
/// "Column", @ref dftracer::utils::dataframe::OwnedDataFrame "OwnedDataFrame"
/// and @ref dftracer::utils::dataframe::FrameBuilder "FrameBuilder" live in the
/// dataframe library, which a plugin links, and are spelled here as this
/// namespace spells them. So do the two borrowed views over a frame the host
/// owns (@ref ColumnView, @ref FrameView), the column type @ref TypeId and the
/// unit a time column is read in (@ref TimeUnit).
namespace dftracer::utils::plugins {

using dataframe::Column;
using dataframe::ColumnView;
using dataframe::FrameBuilder;
using dataframe::FrameView;
using dataframe::OwnedDataFrame;
using dataframe::ScalarView;
using dataframe::TimeUnit;
using dataframe::TypeId;

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H
