#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/index/store/index_database.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::schemas::dft {

/// Adds the count granules of `path` ("cat", "name" or "pid_tid") of the
/// `counts` kind into `into`. The caller checks that the extension is current.
void add_path_counts(const index::store::IndexDatabase& db, int file_id,
                     std::string_view path,
                     dstd::StringViewMap<std::uint64_t>& into);

/// Each file's chunk statistics merged, with the category, name and pid:tid
/// counts of the `counts` kind. Files without statistics are absent.
ankerl::unordered_dense::map<int, MergedStatisticsResult> merged_statistics(
    const index::store::IndexDatabase& db, const std::vector<int>& file_ids);

}  // namespace dftracer::utils::index::schemas::dft

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H
