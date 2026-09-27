#ifndef DFTRACER_UTILS_TRACE_VIEWS_ENGINE_AGG_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_ENGINE_AGG_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_scan.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::trace::views::detail {

// The dataframe-engine aggregation as a Fold on the fused scan: one mergeable
// AggState per slice, accumulated per batch through build_agg_input_frame (the
// same derivation the streaming collect uses), merged with agg_merge. The
// per-event phase/metadata/query filter mirrors the collect path so the same
// events reach the aggregation.
class EngineAggFold : public Fold {
   public:
    EngineAggFold(const ViewPlan& plan,
                  const dftracer::utils::StringIntern& intern,
                  bool apply_query = false)
        : plan_(&plan),
          intern_(&intern),
          spec_(make_agg_input_spec(plan)),
          lowered_(dftracer::utils::dataframe::lower_group_aggs(spec_.gaggs)),
          phase_target_(agg_phase_target(plan)),
          keep_metadata_(metadata_rows(plan)),
          apply_query_(apply_query && plan.query.has_value()),
          resolver_(spec_.transform_wants_resolver ? ensure_resolver(plan)
                                                   : nullptr),
          state_(dftracer::utils::dataframe::agg_new(lowered_.specs,
                                                     spec_.dyn_specs)) {}

    bool accepts(const ScanShape&) const override { return true; }

    // Marking too few args silently drops fields, so stay conservative.
    bool needs_args() const override {
        if (plan_->auto_numeric_metrics || !plan_->computed.empty())
            return true;
        for (const auto& gk : plan_->group_by)
            if (gk.kind == GroupKey::Kind::Arg ||
                gk.kind == GroupKey::Kind::Field)
                return true;
        for (const auto& spec : plan_->agg) {
            if (!arg_free_field(spec.field)) return true;
            if (spec.op == AggOp::ArgMax && !arg_free_field(spec.by))
                return true;
        }
        // A predicated collect filters per event, so the scan must carry any
        // arg field the predicate reads (cat/name/pid/... are arg-free).
        if (apply_query_ && plan_->query)
            for (std::string_view f : plan_->query->fields())
                if (!arg_free_field(std::string(f))) return true;
        return false;
    }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<EngineAggFold>(*plan_, *intern_, apply_query_);
    }

    void step(const FoldBatch& batch) override {
        std::vector<FoldEvent> keep =
            select_events(batch, [&](const FoldEvent& ev) {
                if (ev.phase == RecordPhase::METADATA && !keep_metadata_ &&
                    phase_target_ != RecordPhase::METADATA)
                    return false;
                if (phase_target_ != RecordPhase::UNKNOWN &&
                    ev.phase != phase_target_)
                    return false;
                return !apply_query_ ||
                       pod_matches(*plan_->query, ev, *intern_, qmap_);
            });
        if (keep.empty()) return;
        dftracer::utils::dataframe::DataFrame f =
            build_agg_input_frame(keep, *intern_, spec_, resolver_);
        dftracer::utils::dataframe::agg_accumulate_chunk(
            *state_, f, spec_.group_key_names, lowered_.value_names,
            spec_.emit_dyn ? spec_.dyn_prefix : std::string());
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    void merge(Fold& other) override {
        auto& o = static_cast<EngineAggFold&>(other);
        dftracer::utils::dataframe::agg_merge(*state_, *o.state_);
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    /// The merged partial; the caller finalizes, serializes, or merges it.
    dftracer::utils::dataframe::AggState& state() { return *state_; }

   private:
    static bool arg_free_field(const std::string& f) {
        return f.empty() || f == "ts" || f == "dur" || f == "te" ||
               f == "cat" || f == "name" || f == "pid" || f == "tid" ||
               f == "fhash" || f == "hhash";
    }

    const ViewPlan* plan_;
    const dftracer::utils::StringIntern* intern_;
    AggInputSpec spec_;
    dftracer::utils::dataframe::LoweredGroupAggs lowered_;
    RecordPhase phase_target_;
    bool keep_metadata_;
    bool apply_query_ = false;
    const dftracer::utils::index::plan::GroupResolver* resolver_ = nullptr;
    dftracer::utils::dataframe::AggStatePtr state_;
    duql::ValueMap qmap_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_ENGINE_AGG_FOLD_H
