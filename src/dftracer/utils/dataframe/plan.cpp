// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dataframe/plan.h>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::dataframe {

namespace {
dataframe::DataFrame share_batch(const dataframe::DataFrame& b) {
    dataframe::DataFrame out;
    out.names = b.names;
    out.columns.reserve(b.columns.size());
    for (const dataframe::Series& c : b.columns)
        out.columns.push_back(c.share());
    return out;
}
}  // namespace

dataframe::DataFrame execute(const QueryPlan& plan,
                             const dataframe::DataFrame& input) {
    dataframe::DataFrame cur =
        plan.where ? dataframe::filter(input, evaluate_mask(*plan.where, input))
                   : share_batch(input);
    if (!plan.group_by.empty())
        cur = dataframe::group_by(cur, plan.group_by, plan.aggs);
    if (!plan.select.empty()) cur = dataframe::select(cur, plan.select);
    if (!plan.order_by.empty())
        cur = dataframe::sort_by(cur, plan.order_by, plan.descending);
    if (plan.limit >= 0) cur = dataframe::head(cur, plan.limit);
    return cur;
}

}  // namespace dftracer::utils::dataframe
