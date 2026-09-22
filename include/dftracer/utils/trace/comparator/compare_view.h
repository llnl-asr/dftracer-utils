#ifndef DFTRACER_UTILS_TRACE_COMPARATOR_COMPARE_VIEW_H
#define DFTRACER_UTILS_TRACE_COMPARATOR_COMPARE_VIEW_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::comparator {

/**
 * @brief A comparison of two Views, expressed entirely on the View/DataFrame
 * engine.
 *
 * collect() aggregates the baseline and variant with the same group_by + agg
 * IN PARALLEL (one scan each, run concurrently), joins their result DataFrames
 * on the group key, and appends per metric `m`:
 *   - `delta_<m>`  = variant - baseline
 *   - `pct_<m>`    = 100 * (delta / baseline)
 *
 * View already owns index resolution, the parallel pruned scan, and the
 * aggregation; the delta is just dataframe column math. There is no bespoke
 * aggregator or comparison-tree machinery. Like StatsView, a CompareView is a
 * cheap description of two sources, not a materialized result.
 */
class CompareView {
   public:
    static CompareView of(views::View baseline, views::View variant) {
        return CompareView(std::move(baseline), std::move(variant));
    }

    CompareView& group_by(std::vector<views::GroupKey> keys) {
        group_by_ = std::move(keys);
        return *this;
    }
    CompareView& agg(std::vector<views::AggSpec> specs) {
        agg_ = std::move(specs);
        return *this;
    }

    /// The comparison DataFrame: the group key columns, `l_<m>`/`r_<m>` for
    /// each aggregated metric, plus `delta_<m>` and `pct_<m>`.
    coro::CoroTask<dataframe::DataFrame> collect() const {
        // Both sides aggregate concurrently: two pruned scans, one when_all.
        auto [base, variant] = co_await coro::when_all(
            baseline_.group_by(group_by_).agg(agg_).collect().collect(),
            variant_.group_by(group_by_).agg(agg_).collect().collect());
        co_return compare_batches(base, variant,
                                  static_cast<std::int64_t>(group_by_.size()));
    }

    /// FULL-join two aggregation results on their first `n_key` group-key
    /// columns and append `delta_<m>`/`pct_<m>` for each numeric `l_`/`r_`
    /// metric pair.
    static dataframe::DataFrame compare_batches(
        const dataframe::DataFrame& base, const dataframe::DataFrame& variant,
        std::int64_t n_key) {
        return base.compare_agg(variant, n_key);
    }

   private:
    CompareView(views::View baseline, views::View variant)
        : baseline_(std::move(baseline)), variant_(std::move(variant)) {}

    views::View baseline_;
    views::View variant_;
    std::vector<views::GroupKey> group_by_;
    std::vector<views::AggSpec> agg_;
};

}  // namespace dftracer::utils::trace::comparator

#endif  // DFTRACER_UTILS_TRACE_COMPARATOR_COMPARE_VIEW_H
