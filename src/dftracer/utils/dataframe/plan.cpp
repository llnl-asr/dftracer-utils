// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dataframe/plan.h>

namespace dftracer::utils::dataframe {

namespace {
DataFrame share_batch(const DataFrame& b) {
    DataFrame out;
    out.names = b.names;
    out.columns.reserve(b.columns.size());
    for (const Series& c : b.columns) out.columns.push_back(c.share());
    return out;
}
}  // namespace

dstd::Result<DataFrame> execute(const QueryPlan& plan, const DataFrame& input) {
    DataFrame cur;
    if (plan.where) {
        const dstd::Result<Series> mask = evaluate_mask(*plan.where, input);
        if (!mask) return mask.error();
        cur = filter(input, *mask);
    } else {
        cur = share_batch(input);
    }
    if (!plan.group_by.empty()) {
        dstd::Result<DataFrame> grouped =
            group_by(cur, plan.group_by, plan.aggs);
        if (!grouped) return grouped.error();
        cur = std::move(*grouped);
    }
    if (!plan.select.empty()) {
        dstd::Result<DataFrame> projected = select(cur, plan.select);
        if (!projected) return projected.error();
        cur = std::move(*projected);
    }
    if (!plan.order_by.empty()) {
        dstd::Result<DataFrame> sorted =
            sort_by(cur, plan.order_by, plan.descending);
        if (!sorted) return sorted.error();
        cur = std::move(*sorted);
    }
    if (plan.limit >= 0) cur = head(cur, plan.limit);
    return cur;
}

}  // namespace dftracer::utils::dataframe
