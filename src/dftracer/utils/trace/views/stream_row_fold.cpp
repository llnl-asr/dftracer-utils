#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views::detail {

std::uint64_t morsel_bytes(const dataframe::Morsel& m) {
    std::uint64_t total = 0;
    for (const dataframe::Series& c : m.columns) {
        const dataframe::TypeId t = c.type();
        if (t == dataframe::TypeId::String || t == dataframe::TypeId::Binary) {
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
    std::vector<FoldEvent> events =
        select_events(batch, [&](const FoldEvent& ev) {
            return ev.phase != RecordPhase::UNKNOWN &&
                   (keep_metadata_ || ev.phase != RecordPhase::METADATA);
        });
    if (events.empty()) return;

    const ColumnSpec spec{select_, time_scale_, resolver_.get(), emit_dyn_};
    dataframe::Morsel m = events_to_morsel(events, intern_, spec);

    const std::uint64_t bytes = morsel_bytes(m);
    pending_task_.emplace(send(std::move(m), bytes));
    pending_ = reinterpret_cast<::dftu_task*>(&*pending_task_);
}

}  // namespace dftracer::utils::trace::views::detail
