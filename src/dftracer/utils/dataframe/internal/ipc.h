#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_IPC_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_IPC_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/dataframe/dataframe.h>

#include <optional>
#include <string_view>

namespace dftracer::utils::dataframe {

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
/// The frame an Arrow IPC stream of one record batch (DataFrame::to_ipc)
/// holds, or nullopt when `bytes` is not one.
std::optional<DataFrame> frame_from_ipc(std::string_view bytes);
#endif

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_IPC_H
