// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_STATISTICS_DETAIL_STATS_VIEW_H
#define DFTRACER_UTILS_TRACE_STATISTICS_DETAIL_STATS_VIEW_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/statistics/detailed_statistics.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstddef>

namespace dftracer::utils::duql {
class Query;
}

namespace dftracer::utils::trace::statistics {

/// What a @ref DetailStatsView collection needs: the group-by dimensions, the
/// optional name/category event filters, and an optional query that prunes
/// chunks in the scan. Pointers are borrowed and must outlive the collect()
/// call.
struct DetailNeeds {
    const dstd::Vector<dstd::String>* group_by = nullptr;
    const dstd::Vector<dstd::String>* filter_names = nullptr;
    const dstd::Vector<dstd::String>* filter_categories = nullptr;
    const duql::Query* query = nullptr;  ///< chunk pruning; null = scan all
    std::size_t slot_count = 0;  ///< parallel scan width; 0 = a small default
};

/// A detailed-statistics view over one or more trace files: the per-group
/// duration/I/O distribution report expressed as a fold over a View's
/// index-pruned parallel scan. Holds a source, not a materialized result.
class DetailStatsView {
   public:
    static DetailStatsView from_file(dstd::String file,
                                     dstd::String index = "") {
        DetailStatsView s;
        s.file_ = std::move(file);
        s.index_ = std::move(index);
        return s;
    }
    static DetailStatsView from_files(dstd::Vector<views::ViewFile> files) {
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

    /// Fold the scan into a @ref DetailedStatistics, honoring the group-by and
    /// name/category filters. chunks_scanned/chunks_skipped/events_scanned are
    /// filled from the scan stats.
    coro::CoroTask<DetailedStatistics> collect(const DetailNeeds& needs) const;

   private:
    dstd::String file_, index_;
    dstd::Vector<views::ViewFile> files_;
    bool multi_ = false;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_DETAIL_STATS_VIEW_H
