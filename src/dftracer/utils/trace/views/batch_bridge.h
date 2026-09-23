#ifndef DFTRACER_UTILS_TRACE_VIEWS_BATCH_BRIDGE_H
#define DFTRACER_UTILS_TRACE_VIEWS_BATCH_BRIDGE_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dftracer::utils::trace::views::detail {

class GroupResolver;

/// The value on a pipeline edge: a FoldBatch at the leaf (a zero-copy view over
/// the scan, valid for one step call only, hence held by reference), a Morsel
/// once materialized.
using Batch =
    std::variant<std::reference_wrapper<const FoldBatch>, dataframe::Morsel>;

/// Which columns a scan batch materializes into. `select` holds build_row_frame
/// select tokens (empty = every column); `resolver` is needed only when
/// `select` names a resolved.*/r.* field; `emit_dyn` adds the auto-numeric
/// per-arg columns.
struct ColumnSpec {
    std::vector<std::string> select;
    double time_scale = 1.0;
    const GroupResolver* resolver = nullptr;
    bool emit_dyn = false;
};

/// Materialize `events` into columns: build_row_frame's order for
/// `spec.select`, then the auto-numeric dyn columns appended when
/// `spec.emit_dyn`.
dataframe::DataFrame events_to_frame(
    const std::vector<FoldEvent>& events,
    const dftracer::utils::StringIntern& intern, const ColumnSpec& spec);

/// The same columns as a morsel: the fixed ones positional, the dyn ones out of
/// band in Morsel::dyn_* so the fixed layout stays the same batch to batch.
/// `intern` is shared with the morsel, which keeps it alive.
dataframe::Morsel events_to_morsel(
    const std::vector<FoldEvent>& events,
    std::shared_ptr<dftracer::utils::StringIntern> intern,
    const ColumnSpec& spec);

/// The records of `batch` that `keep` accepts, in scan order. `keep` is called
/// once per event and may carry side effects (metadata harvesting); it is a
/// template parameter so the per-event test inlines into the scan loop.
template <class Keep>
std::vector<FoldEvent> select_events(const FoldBatch& batch, Keep keep) {
    std::vector<FoldEvent> kept;
    kept.reserve(batch.events.size());
    for (const FoldEvent& ev : batch.events)
        if (keep(ev)) kept.push_back(ev);
    return kept;
}

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_BATCH_BRIDGE_H
