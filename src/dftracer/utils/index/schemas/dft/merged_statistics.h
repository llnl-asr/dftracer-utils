#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/index/store/index_database.h>

#include <vector>

namespace dftracer::utils::index::schemas::dft {

/// Each file's chunk statistics merged, with the category, name and pid:tid
/// counts of the `counts` kind. Files without statistics are absent.
ankerl::unordered_dense::map<int, MergedStatisticsResult> merged_statistics(
    const index::store::IndexDatabase& db, const std::vector<int>& file_ids);

}  // namespace dftracer::utils::index::schemas::dft

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_MERGED_STATISTICS_H
