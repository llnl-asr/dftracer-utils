// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_TZ_CURSOR_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_TZ_CURSOR_H

#include <dftracer/utils/common/calendar.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dstd/int128.h>

#include <cstdint>
#include <limits>

// The local time of a zone-aware Timestamp column: which zone a column is
// in, and a cursor that gives the offset of a row without a search while the
// rows stay in one stretch of the zone.

namespace dftracer::utils::dataframe::tz {

/// The zone of a Timestamp column. `zone` is null when the column needs no
/// offset: naive, or in a zone that is UTC for all time. False, with the
/// failure recorded, when the column's zone is not in the database.
inline bool column_zone(const dftu_series& v, const Zone** zone) {
    *zone = nullptr;
    clear_failure();
    if (v.type != TypeId::Timestamp) return true;
    const dstd::StringView name = v.timezone();
    if (name.empty()) return true;
    auto found = tz::zone(name);
    if (!found) {
        record_failure(found.error());
        return false;
    }
    std::int32_t offset = 0;
    if ((*found)->fixed_offset(&offset) && offset == 0) return true;
    *zone = *found;
    return true;
}

/// Offsets of ticks in one unit, for rows read in any order; consecutive rows
/// in one stretch of the zone cost two comparisons.
class OffsetCursor {
   public:
    OffsetCursor(const Zone& zone, std::int64_t per_second)
        : zone_(zone), per_(per_second) {}

    /// The stretch around `tick`, valid until the next call.
    const Segment& at(std::int64_t tick) {
        if (tick < lo_ || tick >= hi_) refresh(tick);
        return seg_;
    }

    /// The wall time of `tick` in ticks, false when it does not fit Int64.
    bool local(std::int64_t tick, std::int64_t* out) {
        const std::int32_t off = at(tick).offset;
        const dstd::int128_t wide = static_cast<dstd::int128_t>(tick) +
                                    static_cast<dstd::int128_t>(off) * per_;
        if (wide < std::numeric_limits<std::int64_t>::min() ||
            wide > std::numeric_limits<std::int64_t>::max())
            return false;
        *out = static_cast<std::int64_t>(wide);
        return true;
    }

   private:
    void refresh(std::int64_t tick) {
        seg_ = zone_.segment_at(common::floor_div(tick, per_), seg_.index);
        lo_ = scale(seg_.begin);
        hi_ = scale(seg_.end);
    }

    std::int64_t scale(std::int64_t seconds) const {
        constexpr std::int64_t LOW = std::numeric_limits<std::int64_t>::min();
        constexpr std::int64_t HIGH = std::numeric_limits<std::int64_t>::max();
        if (seconds <= LOW / per_) return LOW;
        if (seconds >= HIGH / per_) return HIGH;
        return seconds * per_;
    }

    const Zone& zone_;
    std::int64_t per_;
    Segment seg_;
    std::int64_t lo_ = 1;
    std::int64_t hi_ = 0;
};

}  // namespace dftracer::utils::dataframe::tz

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_TZ_CURSOR_H
