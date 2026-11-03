// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/index_rule_fold.h>
#include <dftracer/utils/trace/views/view_agg_partials.h>

#include <utility>

namespace dftracer::utils::trace::views::detail {

IndexRuleFold::Rules::Rules(const ViewPlan& p, AggQuery q,
                            dftracer::utils::dstd::StringIntern& read,
                            bool filter)
    : texts(),
      query(std::move(q)),
      events(query, read, texts),
      plan(&p),
      apply_query(filter) {}

IndexRuleFold::IndexRuleFold(Key, dstd::SharedPtr<Rules> rules)
    : rules_(std::move(rules)), folder_(rules_->events, rows_, stats_) {}

dstd::UniquePtr<IndexRuleFold> IndexRuleFold::make(
    const ViewPlan& plan, dftracer::utils::dstd::StringIntern& scan_intern,
    bool apply_query) {
    auto query = trace_fold_query(plan);
    if (!query) return nullptr;
    return make(plan, std::move(*query), scan_intern, apply_query);
}

dstd::UniquePtr<IndexRuleFold> IndexRuleFold::make(
    const ViewPlan& plan, AggQuery query,
    dftracer::utils::dstd::StringIntern& scan_intern, bool apply_query) {
    auto rules = dstd::make_shared<Rules>(plan, std::move(query), scan_intern,
                                          apply_query);
    return dstd::make_unique<IndexRuleFold>(Key{}, std::move(rules));
}

dstd::UniquePtr<Fold> IndexRuleFold::slice() const {
    return dstd::make_unique<IndexRuleFold>(Key{}, rules_);
}

void IndexRuleFold::step(const FoldBatch& batch) {
    const bool filter = rules_->apply_query && rules_->plan->query.has_value();
    for (const FoldEvent& ev : batch.events) {
        if (filter &&
            !pod_matches(*rules_->plan->query, ev, rules_->events.read, qmap_))
            continue;
        folder_.add_event(ev, nullptr);
    }
}

void IndexRuleFold::merge(Fold& other) {
    auto& o = static_cast<IndexRuleFold&>(other);
    rows_.reserve(rows_.size() + o.rows_.size());
    for (auto& [key, partial] : o.rows_) {
        auto it = rows_.find(key);
        if (it == rows_.end())
            rows_.emplace(key, std::move(partial));
        else
            it->second.merge(partial);
    }
    o.rows_ = AggQueryResult::Rows();
    stats_.events_read += o.stats_.events_read;
}

coro::CoroTask<bool> IndexRuleFold::finalize(const CoverageSet&) {
    state_ = seed_rows(*rules_->plan, rules_->query, rules_->texts, rows_);
    rows_ = AggQueryResult::Rows();
    co_return true;
}

}  // namespace dftracer::utils::trace::views::detail
