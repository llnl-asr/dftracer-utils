// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/query/filter_duql.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/explain_abi.h>
#include <dftracer/utils/plugins/host/plan_abi.h>
#include <dftracer/utils/plugins/host/plugin_task.h>
#include <dftracer/utils/plugins/host/provider_source.h>

#include <cmath>

namespace dftracer::utils::plugins::host {

dstd::Result<dstd::Optional<dataframe::ScanPlan>> run_plan(
    const PlanHooks& hooks, const dataframe::ScanRequest& req,
    dstd::Span<const dstd::String> names) {
    if (!hooks.plan && !hooks.plan_duql)
        return dstd::Optional<dataframe::ScanPlan>{};

    dataframe::query::DuqlFilters duql;
    if (hooks.plan_duql) {
        const dstd::Span<const dstd::String> columns =
            req.projection.empty()
                ? names
                : dstd::Span<const dstd::String>(req.projection.data(),
                                                 req.projection.size());
        duql = dataframe::query::filters_to_duql(req.filters, columns);
    }
    const bool say_duql = hooks.plan_duql && !duql.index.empty();
    if (!say_duql && !hooks.plan) return dstd::Optional<dataframe::ScanPlan>{};

    const AbiScanRequest creq(req);
    dstd::Vector<std::int32_t> votes(req.filters.size(), DFTU_PUSHED_NO);
    ::dftu_plan_result out{};
    out.struct_size = sizeof(out);
    out.votes = votes.data();
    out.cost = -1.0;
    out.est_rows = -1;
    out.token = 0;

    plugins::AbiContext ctx({}, hooks.owner, req.counters.get());
    dftu_status status = 0;
    if (say_duql) {
        ::dftu_plan_duql q{};
        q.struct_size = sizeof(q);
        q.text = duql.text.c_str();
        q.text_len = duql.text.size();
        q.filter_index = duql.index.data();
        q.n_filters = static_cast<std::int32_t>(duql.index.size());
        status = hooks.plan_duql(ctx.get(), hooks.self, creq.get(), &q, &out);
    } else {
        status = hooks.plan(ctx.get(), hooks.self, creq.get(), &out);
    }
    if (status < 0) return plugin_error(ctx, status, hooks.what);

    dataframe::ScanPlan plan;
    plan.votes.reserve(votes.size());
    for (const std::int32_t v : votes) plan.votes.push_back(plugin_pushed(v));
    plan.cost = std::isnan(out.cost) || out.cost < 0 ? -1.0 : out.cost;
    plan.est_rows = out.est_rows < 0 ? -1 : out.est_rows;
    plan.token = out.token;
    return dstd::Optional<dataframe::ScanPlan>(std::move(plan));
}

dataframe::ScanRequest with_plan(
    const dataframe::ScanRequest& req,
    const dstd::Result<dstd::Optional<dataframe::ScanPlan>>& planned) {
    dataframe::ScanRequest out = req;
    if (planned && planned->has_value()) out.plan = **planned;
    return out;
}

dstd::Vector<dataframe::ExplainLine> plan_lines(
    const dstd::Result<dstd::Optional<dataframe::ScanPlan>>& planned) {
    dstd::Vector<dataframe::ExplainLine> lines;
    if (!planned) {
        lines.push_back({"plan_error", planned.error().format()});
        return lines;
    }
    if (!planned->has_value()) return lines;
    const dataframe::ScanPlan& plan = **planned;
    const char* const name[] = {"no", "inexact", "exact"};
    dstd::String votes;
    dstd::fmt_append_join<",">(votes, plan.votes,
                               [&](dstd::String& o, const dataframe::Pushed v) {
                                   o += name[static_cast<std::size_t>(v)];
                               });
    lines.push_back({"plan_votes", votes.empty() ? "none" : votes});
    lines.push_back({"plan_cost", plan.cost < 0
                                      ? dstd::String("unknown")
                                      : dstd::double_text(plan.cost)});
    lines.push_back({"plan_est_rows", plan.est_rows < 0
                                          ? dstd::String("unknown")
                                          : dstd::int_text(plan.est_rows)});
    return lines;
}

AbiPlan::AbiPlan(const dstd::Optional<dataframe::ScanPlan>& plan)
    : result_{}, present_(plan.has_value()) {
    if (!plan) return;
    votes_.reserve(plan->votes.size());
    for (const dataframe::Pushed v : plan->votes)
        votes_.push_back(static_cast<std::int32_t>(v));
    result_.struct_size = sizeof(result_);
    result_.votes = votes_.data();
    result_.cost = plan->cost;
    result_.est_rows = plan->est_rows;
    result_.token = plan->token;
}

}  // namespace dftracer::utils::plugins::host
