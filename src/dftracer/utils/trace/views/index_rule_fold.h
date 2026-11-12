// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_INDEX_RULE_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_INDEX_RULE_FOLD_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/trace/agg_event_fold.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <utility>

namespace dftracer::utils::trace::views::detail {

class IndexRuleFold : public Fold {
   public:
    /// Folds every event of `plan` by the rules of the index fold, in the ids
    /// of `scan_intern`. nullptr when the plan stays on the frame path; the
    /// error is that of an index that cannot be read.
    static dstd::Result<dstd::UniquePtr<IndexRuleFold>> make(
        const ViewPlan& plan, dftracer::utils::dstd::StringIntern& scan_intern,
        bool apply_query);

    /// As above, with the query `trace_fold_query(plan)` returned.
    static dstd::Result<dstd::UniquePtr<IndexRuleFold>> make(
        const ViewPlan& plan, AggQuery query,
        dftracer::utils::dstd::StringIntern& scan_intern, bool apply_query);

    bool accepts(const ScanShape&) const override { return true; }

    /// A pre-aggregated record carries its counts in its args.
    bool needs_args() const override { return true; }

    dstd::UniquePtr<Fold> slice() const override;

    dstd::Result<void> step(const FoldBatch& batch) override;
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    dstd::Result<void> merge(Fold& other) override;

    coro::CoroTask<bool> finalize(const CoverageSet&) override;

    /// The merged state, valid after finalize.
    dftracer::utils::dataframe::AggState& state() { return *state_; }

    /// Hands the state over. The fold takes no more events afterwards.
    dftracer::utils::dataframe::AggStatePtr take_state() {
        return std::move(state_);
    }

   private:
    struct Key {
        explicit Key() = default;
    };

    struct Rules {
        Rules(const ViewPlan& plan, AggInputSpec spec, AggQuery q,
              dftracer::utils::dstd::StringIntern& read, bool apply_query);

        dftracer::utils::dstd::StringIntern texts;
        AggQuery query;
        AggEventRules events;
        AggInputSpec spec;
        const ViewPlan* plan;
        bool apply_query;
    };

   public:
    IndexRuleFold(Key, dftracer::utils::dstd::SharedPtr<Rules> rules);

   private:
    dftracer::utils::dstd::SharedPtr<Rules> rules_;
    AggQueryResult::Rows rows_;
    AggQueryStats stats_;
    AggEventFolder folder_;
    dftracer::utils::dataframe::AggStatePtr state_;
    duql::ValueMap qmap_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_INDEX_RULE_FOLD_H
