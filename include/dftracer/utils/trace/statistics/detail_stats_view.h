#ifndef DFTRACER_UTILS_TRACE_STATISTICS_DETAIL_STATS_VIEW_H
#define DFTRACER_UTILS_TRACE_STATISTICS_DETAIL_STATS_VIEW_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/trace/statistics/detailed_statistics.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::duql {
class Query;
}

namespace dftracer::utils::trace::statistics {

/// What a DetailStatsView collection needs: the group-by dimensions, the
/// optional name/category event filters, and an optional query that prunes
/// chunks in the scan. Pointers are borrowed and must outlive the collect()
/// call.
struct DetailNeeds {
    const std::vector<std::string>* group_by = nullptr;
    const std::vector<std::string>* filter_names = nullptr;
    const std::vector<std::string>* filter_categories = nullptr;
    const duql::Query* query = nullptr;  ///< chunk pruning; null = scan all
    std::size_t num_slots = 0;  ///< parallel scan width; 0 = a small default
};

/// A detailed-statistics view over one or more trace files: the per-group
/// duration/I/O distribution report expressed as a fold over a View's
/// index-pruned parallel scan. Holds a source, not a materialized result.
class DetailStatsView {
   public:
    static DetailStatsView from_file(std::string file, std::string index = "") {
        DetailStatsView s;
        s.file_ = std::move(file);
        s.index_ = std::move(index);
        return s;
    }
    static DetailStatsView from_files(std::vector<views::ViewFile> files) {
        DetailStatsView s;
        s.files_ = std::move(files);
        s.multi_ = true;
        return s;
    }

    /// A fresh View over this source.
    views::View view() const {
        return multi_ ? views::View::from_files(files_)
                      : views::View::from_file(file_, index_);
    }

    /// Fold the scan into a DetailedStatistics, honoring the group-by and
    /// name/category filters. chunks_scanned/chunks_skipped/events_scanned are
    /// filled from the scan stats.
    coro::CoroTask<DetailedStatistics> collect(const DetailNeeds& needs) const;

   private:
    std::string file_, index_;
    std::vector<views::ViewFile> files_;
    bool multi_ = false;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_DETAIL_STATS_VIEW_H
