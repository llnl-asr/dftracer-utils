// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_COUNTER_FORMAT_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_COUNTER_FORMAT_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan.h>

// Emitting aggregate rows/groups as dftracer ph="C" counter events: group
// columns map to name/cat/pid/tid/ts, everything else becomes args.
namespace dftracer::utils::trace::views::detail {

// One aggregate group -> one ph="C" counter event JSON line. `keys` aligns to
// `group_cols` (nullopt for a null key), `values` to `value_cols`. The group
// columns `entity_col` and `lane_col` give pid and tid, as
// trace::index::entity_id.
dstd::String counter_line(
    const dstd::Vector<dstd::String>& group_cols,
    const dstd::Vector<dstd::Optional<dstd::String>>& keys,
    const dstd::Vector<dstd::String>& value_cols,
    const dstd::Vector<double>& values, const dstd::String& entity_col,
    const dstd::String& lane_col);

// Emit ph="C" counter events from a merged engine AggState (the distributed
// counter-partial merge). Byte-matches emit_group_counter: finalizes the state,
// then writes one event per group with its own args (spec value columns then
// the present dyn columns, skipping args a group did not see).
dstd::Result<void> emit_counters_from_state(
    const dftracer::utils::dataframe::AggState& state, const ViewPlan& plan,
    ExportSink& sink);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_COUNTER_FORMAT_H
