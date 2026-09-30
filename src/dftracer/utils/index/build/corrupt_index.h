#ifndef DFTRACER_UTILS_INDEX_BUILD_CORRUPT_INDEX_H
#define DFTRACER_UTILS_INDEX_BUILD_CORRUPT_INDEX_H

#include <set>
#include <string>

namespace dftracer::utils::index::build {

/// The error text for a corrupt index a build cannot repair: it names every
/// index directory and says to delete it and build again. `tier_cleared` says
/// the build already cleared the aggregation tier once and still failed.
/// Nothing is deleted here; the caller may still use the directory.
inline std::string corrupt_index_message(const std::set<std::string>& roots,
                                         const std::string& cause,
                                         bool tier_cleared) {
    std::string dirs;
    for (const auto& root : roots) {
        if (!dirs.empty()) dirs += ", ";
        dirs += root;
    }
    return "the index at " + dirs + " is corrupt" +
           (tier_cleared
                ? " and clearing its aggregation tier did not repair it"
                : "") +
           " (" + cause + "); delete that directory and build the index again";
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_CORRUPT_INDEX_H
