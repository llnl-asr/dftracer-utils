// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_BORROW_RELEASE_PIN_H
#define DFTRACER_UTILS_PLUGINS_HOST_BORROW_RELEASE_PIN_H

#include <dftracer/utils/dataframe/internal/borrow_release_pin.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/plugins/host/plugin_library.h>

namespace dftracer::utils::plugins::host {

/// Track the plugin library at @p image_base so a column that borrows one of
/// its buffers keeps it mapped until the column is freed, however long after
/// the plugin set unloaded. Holds a weak reference only: the plugin set owns
/// the library.
void track_plugin_library(void* image_base,
                          const dstd::SharedPtr<PluginLibrary>& library);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_BORROW_RELEASE_PIN_H
