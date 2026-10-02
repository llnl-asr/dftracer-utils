#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views::detail {

std::uint64_t morsel_bytes(const dataframe::Morsel& m) {
    std::uint64_t total = 0;
    for (const dataframe::Series& c : m.columns) {
        const dataframe::TypeId t = c.type();
        if (c.encoding() == dataframe::Encoding::Dictionary ||
            c.encoding() == dataframe::Encoding::View) {
            total += static_cast<std::uint64_t>(
                dftu_series_buffer_bytes(c.handle()));
        } else if (t == dataframe::TypeId::String ||
                   t == dataframe::TypeId::Binary) {
            const std::int32_t* off = c.offsets();
            total += off ? static_cast<std::uint64_t>(off[c.length()] - off[0])
                         : static_cast<std::uint64_t>(c.length()) * 16;
        } else if (t == dataframe::TypeId::List ||
                   t == dataframe::TypeId::Struct) {
            total += static_cast<std::uint64_t>(c.length()) * 16;
        } else {
            total += dataframe::buffer_bytes(t, c.length());
        }
    }
    return total;
}

void StreamRowFold::step(const FoldBatch& batch) {
    if (dropped_ && dropped_->load(std::memory_order_relaxed)) return;
    const RecordPhase target =
        branch_ ? agg_phase_target(*branch_) : RecordPhase::UNKNOWN;
    const std::size_t total = batch.events.size();
    std::vector<std::uint8_t> keep(total);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const FoldEvent& ev = batch.events[i];
        const bool k = ev.phase != RecordPhase::UNKNOWN &&
                       (keep_metadata_ || ev.phase != RecordPhase::METADATA) &&
                       (target == RecordPhase::UNKNOWN || ev.phase == target) &&
                       (!branch_ || !branch_->query ||
                        pod_matches(*branch_->query, ev, *intern_, qmap_));
        keep[i] = k;
        kept += k;
    }
    if (kept == 0) return;
    std::vector<FoldEvent> filtered;
    std::span<const FoldEvent> events = batch.events;
    if (kept != total) {
        filtered.reserve(kept);
        for (std::size_t i = 0; i < total; ++i)
            if (keep[i]) filtered.push_back(batch.events[i]);
        events = filtered;
    }

    const ColumnSpec spec{select_, time_scale_, emit_dyn_, by_path_, json_};
    dataframe::Morsel m = events_to_morsel(events, intern_, spec);
    if (ordered_) m.batch_index = static_cast<std::int64_t>(batch.unit.seq);

    const std::uint64_t bytes = morsel_bytes(m);
    pending_task_.emplace(send(std::move(m), bytes));
    pending_ = reinterpret_cast<::dftu_task*>(&*pending_task_);
}

void StreamRowFold::end_unit(const ScanUnit& unit) {
    if (!ordered_ || (dropped_ && dropped_->load(std::memory_order_relaxed)))
        return;
    dataframe::Morsel end;
    end.batch_index = static_cast<std::int64_t>(unit.seq);
    pending_task_.emplace(send(std::move(end), 0));
    pending_ = reinterpret_cast<::dftu_task*>(&*pending_task_);
}

}  // namespace dftracer::utils::trace::views::detail
