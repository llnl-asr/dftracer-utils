// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_CORRUPT_INDEX_H
#define DFTRACER_UTILS_INDEX_BUILD_CORRUPT_INDEX_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::index::build {

/// The error text for a corrupt index a build cannot repair: it names every
/// index directory and says to delete it and build again. `tier_cleared` says
/// the build already cleared the aggregation tier once and still failed.
/// Nothing is deleted here; the caller may still use the directory.
inline dstd::String corrupt_index_message(const dstd::Set<dstd::String>& roots,
                                          const dstd::String& cause,
                                          bool tier_cleared) {
    dstd::String dirs;
    for (const auto& root : roots) {
        if (!dirs.empty()) dirs += ", ";
        dirs += root;
    }
    return dstd::fmt<
        "the index at {} is corrupt{} ({}); delete that directory and build "
        "the index again">(
        dirs,
        tier_cleared ? " and clearing its aggregation tier did not repair it"
                     : "",
        cause);
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_CORRUPT_INDEX_H
