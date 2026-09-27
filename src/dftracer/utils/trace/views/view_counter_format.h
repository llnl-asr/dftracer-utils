#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_COUNTER_FORMAT_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_COUNTER_FORMAT_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <optional>
#include <string>
#include <vector>

// Emitting aggregate rows/groups as dftracer ph="C" counter events: group
// columns map to name/cat/pid/tid/ts, everything else becomes args.
namespace dftracer::utils::trace::views::detail {

// One aggregate group -> one ph="C" counter event JSON line. `keys` aligns to
// `group_cols` (nullopt for a null key), `values` to `value_cols`. The group
// columns `entity_col` and `lane_col` give pid and tid, as index::entity_id.
std::string counter_line(const std::vector<std::string>& group_cols,
                         const std::vector<std::optional<std::string>>& keys,
                         const std::vector<std::string>& value_cols,
                         const std::vector<double>& values,
                         const std::string& entity_col,
                         const std::string& lane_col);

// Emit ph="C" counter events from a merged engine AggState (the distributed
// counter-partial merge). Byte-matches emit_group_counter: finalizes the state,
// then writes one event per group with its own args (spec value columns then
// the present dyn columns, skipping args a group did not see).
void emit_counters_from_state(const dftracer::utils::dataframe::AggState& state,
                              const ViewPlan& plan, ExportSink& sink);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_COUNTER_FORMAT_H
