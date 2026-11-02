// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H

#include <dftracer/utils/index/schemas/dft/statistics.h>
#include <dftracer/utils/index/tables.h>

namespace dftracer::utils::index::schemas::dft {

/// Adds the count granules of `path` ("cat", "name" or "pid_tid") of the
/// `counts` kind into `into`. The caller checks that the extension is current.
common::Result<void> add_path_counts(const Snapshot& snapshot,
                                     const FileRef& file, dstd::StringView path,
                                     dstd::StringViewMap<std::uint64_t>& into);

/// The file's chunk statistics merged, with the category, name and pid:tid
/// counts of the `counts` kind. Nullopt when the file has no statistics.
common::Result<dstd::Optional<MergedStatisticsResult>> merged_statistics(
    const Snapshot& snapshot, const FileRef& file);

}  // namespace dftracer::utils::index::schemas::dft

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H
