#ifndef DFTRACER_UTILS_TRACE_VIEWS_AGGFOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_AGGFOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace dftracer::utils::trace::views::detail {

/// Fields a bootstrap fold can evaluate a predicate on straight from the POD:
/// always-present top-level scalars on a phase-filtered data event. A query
/// touching anything else (args, te, fhash/hhash, resolved columns) cannot be
/// filtered here and must take the indexed path.
inline bool fold_query_field_supported(std::string_view f) {
    return f == "cat" || f == "name" || f == "pid" || f == "tid" || f == "ts" ||
           f == "dur";
}

/// True when every field a query references is POD-evaluable, so the raw-gzip
/// bootstrap can apply the predicate itself instead of deferring to the scan.
inline bool query_evaluable_by_fold(const duql::Query& q) {
    for (std::string_view f : q.fields())
        if (!fold_query_field_supported(f)) return false;
    return true;
}

/// The scan RecordPhase an aggregation plan folds; UNKNOWN (Any) folds every
/// phase with no filter.
inline RecordPhase agg_phase_target(const ViewPlan& plan) {
    if (plan.phase == Phase::Events) return RecordPhase::COMPLETE;
    if (plan.phase == Phase::Counters) return RecordPhase::COUNTER;
    if (plan.phase == Phase::Aggregated) return RecordPhase::AGGREGATED;
    if (plan.phase == Phase::Metadata) return RecordPhase::METADATA;
    return RecordPhase::UNKNOWN;
}

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_AGGFOLD_H
