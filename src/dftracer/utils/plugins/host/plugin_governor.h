// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_GOVERNOR_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_GOVERNOR_H

#include <dftracer/utils/dataframe/internal/governor.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/plugins/abi/providers.h>

#include <algorithm>
#include <cstdint>

namespace dftracer::utils::plugins::host {

// A plugin cursor's state is what its `bytes` slot reports, and it frees it
// through its own `reclaim` slot, called at a morsel boundary on the thread
// that pulls the cursor. A plugin without both slots is not governed.
class PluginGovernor {
   public:
    explicit PluginGovernor(const ::dftu_cursor_vt* vt) {
        if (vt->bytes && vt->reclaim) gov_.emplace("plugin");
    }

    void at_morsel(const ::dftu_cursor_vt* vt, void* self) {
        if (!gov_) return;
        const std::uint64_t held = vt->bytes(self);
        const std::uint64_t want = gov_->check(held);
        if (want == 0) return;
        const std::uint64_t freed = vt->reclaim(self, want);
        gov_->spilled(held - std::min(held, freed));
    }

   private:
    dstd::Optional<dataframe::Governor> gov_;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_GOVERNOR_H
