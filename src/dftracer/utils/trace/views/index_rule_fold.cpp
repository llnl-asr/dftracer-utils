#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/index_rule_fold.h>
#include <dftracer/utils/trace/views/view_agg_partials.h>

#include <memory>
#include <utility>

namespace dftracer::utils::trace::views::detail {

namespace dataframe = dftracer::utils::dataframe;
namespace dstd = dftracer::utils::dstd;

IndexRuleFold::Rules::Rules(const ViewPlan& p, AggQuery q,
                            dftracer::utils::dstd::StringIntern& read,
                            bool filter)
    : texts(),
      query(std::move(q)),
      events(query, read, texts),
      plan(&p),
      apply_query(filter) {}

IndexRuleFold::IndexRuleFold(dstd::SharedPtr<Rules> rules)
    : rules_(std::move(rules)), folder_(rules_->events, rows_, stats_) {}

std::unique_ptr<IndexRuleFold> IndexRuleFold::make(
    const ViewPlan& plan, dftracer::utils::dstd::StringIntern& scan_intern,
    bool apply_query) {
    auto query = trace_fold_query(plan);
    if (!query) return nullptr;
    return make(plan, std::move(*query), scan_intern, apply_query);
}

std::unique_ptr<IndexRuleFold> IndexRuleFold::make(
    const ViewPlan& plan, AggQuery query,
    dftracer::utils::dstd::StringIntern& scan_intern, bool apply_query) {
    auto rules = std::make_shared<Rules>(plan, std::move(query), scan_intern,
                                         apply_query);
    return std::unique_ptr<IndexRuleFold>(new IndexRuleFold(std::move(rules)));
}

std::unique_ptr<Fold> IndexRuleFold::slice() const {
    return std::unique_ptr<Fold>(new IndexRuleFold(rules_));
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
