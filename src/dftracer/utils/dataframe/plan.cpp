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

DataFrame execute(const QueryPlan& plan, const DataFrame& input) {
    DataFrame cur = plan.where
                        ? filter(input, evaluate_mask(*plan.where, input))
                        : share_batch(input);
    if (!plan.group_by.empty()) cur = group_by(cur, plan.group_by, plan.aggs);
    if (!plan.select.empty()) cur = select(cur, plan.select);
    if (!plan.order_by.empty())
        cur = sort_by(cur, plan.order_by, plan.descending);
    if (plan.limit >= 0) cur = head(cur, plan.limit);
    return cur;
}

}  // namespace dftracer::utils::dataframe
