// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/calendar.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dataframe/internal/tz_cursor.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <cstring>
#include <limits>

// The calendar parts of an instant (the pandas .dt accessor) and rounding to a
// bucket, over a Timestamp / Date32 / Date64 / Duration column (its own unit)
// or a plain Int64 column read in a given unit. A Timestamp column in a
// zone reads its wall time there; every other column reads UTC.

namespace dftracer::utils::dataframe {

namespace {

std::int64_t unit_per_second(TimeUnit u) {
    switch (u) {
        case TimeUnit::Second:
            return 1;
        case TimeUnit::Milli:
            return 1'000;
        case TimeUnit::Micro:
            return 1'000'000;
        case TimeUnit::Nano:
            return 1'000'000'000;
    }
    return 1'000'000;
}

// The unit an input column's values are in: its own for the temporal types,
// `fallback` for an Int64, and days for the dates.
bool resolve_unit(const dftu_series* v, TimeUnit fallback, TimeUnit* unit,
                  bool* days) {
    *days = false;
    switch (v->type) {
        case TypeId::Timestamp:
        case TypeId::Duration:
            *unit = v->time_unit();
            return true;
        case TypeId::Date32:
            *days = true;
            return true;
        case TypeId::Date64:
            *unit = TimeUnit::Milli;
            return true;
        case TypeId::Int64:
            *unit = fallback;
            return true;
        default:
            return false;
    }
}

std::int64_t value_at(const dftu_series& v, std::int64_t i, bool days) {
    if (days) return reinterpret_cast<const std::int32_t*>(v.data.data())[i];
    return reinterpret_cast<const std::int64_t*>(v.data.data())[i];
}

dftu_series* alloc_i64_like(const dftu_series* v) {
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = new_buffer(buffer_bytes(TypeId::Int64, v->length));
    return out;
}

bool valid_row(const dftu_series& v, std::int64_t i) {
    return !v.validity.block() || ((v.validity.data()[i >> 3] >> (i & 7)) & 1);
}

void out_of_range() {
    tz::record_failure(tz::Failure::OutOfRange,
                       "wall time of an instant does not fit Int64");
}

// The parts that read the day number only, as a function of the part so a
// loop over a column computes just the one it needs.
template <dftu_dt_part P>
std::int64_t day_part(std::int64_t day) {
    if constexpr (P == DFTU_DT_YEAR) {
        return common::civil_from_days(day).year;
    } else if constexpr (P == DFTU_DT_MONTH) {
        return common::civil_from_days(day).month;
    } else if constexpr (P == DFTU_DT_DAY) {
        return common::civil_from_days(day).day;
    } else if constexpr (P == DFTU_DT_DAY_OF_WEEK) {
        // 1970-01-01 was a Thursday (3, Monday = 0).
        return common::floor_mod(day + 3, 7);
    } else if constexpr (P == DFTU_DT_DAY_OF_YEAR) {
        const common::CivilDate c = common::civil_from_days(day);
        return day - common::days_from_civil(c.year, 1, 1) + 1;
    } else if constexpr (P == DFTU_DT_QUARTER) {
        return (common::civil_from_days(day).month - 1) / 3 + 1;
    } else if constexpr (P == DFTU_DT_IS_LEAP_YEAR) {
        return common::is_leap_year(common::civil_from_days(day).year) ? 1 : 0;
    } else if constexpr (P == DFTU_DT_DAYS_IN_MONTH) {
        const common::CivilDate c = common::civil_from_days(day);
        return common::days_in_month(c.year, c.month);
    } else if constexpr (P == DFTU_DT_ISO_WEEK || P == DFTU_DT_ISO_YEAR) {
        // ISO 8601: the week holding the year's first Thursday is week 1.
        const std::int64_t wd = common::floor_mod(day + 3, 7);  // Monday = 0
        const std::int64_t thursday = day - wd + 3;
        const common::CivilDate tc = common::civil_from_days(thursday);
        if constexpr (P == DFTU_DT_ISO_YEAR) {
            return tc.year;
        } else {
            const std::int64_t jan1 = common::days_from_civil(tc.year, 1, 1);
            return (thursday - jan1) / 7 + 1;
        }
    } else {
        static_assert(P == DFTU_DT_EPOCH_DAYS);
        return day;
    }
}

// The parts that read the time of day: `in_day` is the tick within the day.
template <dftu_dt_part P>
std::int64_t time_part(std::int64_t in_day, std::int64_t per_second) {
    if constexpr (P == DFTU_DT_HOUR) {
        return in_day / (per_second * 3600);
    } else if constexpr (P == DFTU_DT_MINUTE) {
        return in_day / (per_second * 60) % 60;
    } else if constexpr (P == DFTU_DT_SECOND) {
        return in_day / per_second % 60;
    } else if constexpr (P == DFTU_DT_MILLISECOND) {
        return in_day % per_second * 1'000 / per_second;
    } else if constexpr (P == DFTU_DT_MICROSECOND) {
        return in_day % per_second * 1'000'000 / per_second;
    } else {
        static_assert(P == DFTU_DT_NANOSECOND);
        const std::int64_t frac = in_day % per_second;
        return per_second >= 1'000'000'000
                   ? frac % 1'000
                   : frac * (1'000'000'000 / per_second) % 1'000;
    }
}

constexpr bool is_time_part(dftu_dt_part p) {
    return p == DFTU_DT_HOUR || p == DFTU_DT_MINUTE || p == DFTU_DT_SECOND ||
           p == DFTU_DT_MILLISECOND || p == DFTU_DT_MICROSECOND ||
           p == DFTU_DT_NANOSECOND;
}

// The floor of a division by a positive constant, without a branch.
template <std::int64_t D>
inline std::int64_t floor_div_const(std::int64_t a) {
    const std::int64_t q = a / D;
    return q - ((a - q * D) < 0 ? 1 : 0);
}

// One value of a part, `PS` ticks to the second (a Date32 column counts days:
// `Days`, and its time parts are 0).
template <dftu_dt_part P, std::int64_t PS, bool Days>
inline std::int64_t part_fixed(std::int64_t t) {
    if constexpr (is_time_part(P)) {
        if constexpr (Days) {
            return 0;
        } else {
            return time_part<P>(t - floor_div_const<PS * 86400>(t) * PS * 86400,
                                PS);
        }
    } else if constexpr (Days) {
        return day_part<P>(t);
    } else {
        return day_part<P>(floor_div_const<PS * 86400>(t));
    }
}

template <dftu_dt_part P, std::int64_t PS, bool Days>
void fill_part(const dftu_series& v, std::int64_t* out) {
    const std::int64_t n = v.length;
    if constexpr (Days) {
        const auto* in = reinterpret_cast<const std::int32_t*>(v.data.data());
        for (std::int64_t i = 0; i < n; ++i)
            out[i] = part_fixed<P, PS, true>(in[i]);
    } else {
        const auto* in = reinterpret_cast<const std::int64_t*>(v.data.data());
        for (std::int64_t i = 0; i < n; ++i)
            out[i] = part_fixed<P, PS, false>(in[i]);
    }
}

// The loop of one part, with the unit a compile-time constant too.
template <dftu_dt_part P>
void fill_part_in_unit(const dftu_series& v, std::int64_t per_second, bool days,
                       std::int64_t* out) {
    if (days) return fill_part<P, 1, true>(v, out);
    switch (per_second) {
        case 1:
            return fill_part<P, 1, false>(v, out);
        case 1'000:
            return fill_part<P, 1'000, false>(v, out);
        case 1'000'000:
            return fill_part<P, 1'000'000, false>(v, out);
        default:
            return fill_part<P, 1'000'000'000, false>(v, out);
    }
}

void fill_parts(const dftu_series& v, std::int64_t per_second, bool days,
                dftu_dt_part part, std::int64_t* out) {
    switch (part) {
#define DFTU_FILL_PART(P) \
    case P:               \
        return fill_part_in_unit<P>(v, per_second, days, out)
        DFTU_FILL_PART(DFTU_DT_YEAR);
        DFTU_FILL_PART(DFTU_DT_MONTH);
        DFTU_FILL_PART(DFTU_DT_DAY);
        DFTU_FILL_PART(DFTU_DT_HOUR);
        DFTU_FILL_PART(DFTU_DT_MINUTE);
        DFTU_FILL_PART(DFTU_DT_SECOND);
        DFTU_FILL_PART(DFTU_DT_MILLISECOND);
        DFTU_FILL_PART(DFTU_DT_MICROSECOND);
        DFTU_FILL_PART(DFTU_DT_NANOSECOND);
        DFTU_FILL_PART(DFTU_DT_DAY_OF_WEEK);
        DFTU_FILL_PART(DFTU_DT_DAY_OF_YEAR);
        DFTU_FILL_PART(DFTU_DT_QUARTER);
        DFTU_FILL_PART(DFTU_DT_IS_LEAP_YEAR);
        DFTU_FILL_PART(DFTU_DT_DAYS_IN_MONTH);
        DFTU_FILL_PART(DFTU_DT_ISO_WEEK);
        DFTU_FILL_PART(DFTU_DT_ISO_YEAR);
        DFTU_FILL_PART(DFTU_DT_EPOCH_DAYS);
#undef DFTU_FILL_PART
    }
}

// One value of a part of a wall time in a zone, the part and the unit read at
// run time.
std::int64_t part_of(std::int64_t t, std::int64_t per_second,
                     dftu_dt_part part) {
    const std::int64_t day_size = per_second * 86400;
    switch (part) {
        case DFTU_DT_HOUR:
        case DFTU_DT_MINUTE:
        case DFTU_DT_SECOND:
        case DFTU_DT_MILLISECOND:
        case DFTU_DT_MICROSECOND:
        case DFTU_DT_NANOSECOND: {
            const std::int64_t in_day = common::floor_mod(t, day_size);
            switch (part) {
                case DFTU_DT_HOUR:
                    return time_part<DFTU_DT_HOUR>(in_day, per_second);
                case DFTU_DT_MINUTE:
                    return time_part<DFTU_DT_MINUTE>(in_day, per_second);
                case DFTU_DT_SECOND:
                    return time_part<DFTU_DT_SECOND>(in_day, per_second);
                case DFTU_DT_MILLISECOND:
                    return time_part<DFTU_DT_MILLISECOND>(in_day, per_second);
                case DFTU_DT_MICROSECOND:
                    return time_part<DFTU_DT_MICROSECOND>(in_day, per_second);
                default:
                    return time_part<DFTU_DT_NANOSECOND>(in_day, per_second);
            }
        }
        default:
            break;
    }
    const std::int64_t day = common::floor_div(t, day_size);
    switch (part) {
        case DFTU_DT_YEAR:
            return day_part<DFTU_DT_YEAR>(day);
        case DFTU_DT_MONTH:
            return day_part<DFTU_DT_MONTH>(day);
        case DFTU_DT_DAY:
            return day_part<DFTU_DT_DAY>(day);
        case DFTU_DT_DAY_OF_WEEK:
            return day_part<DFTU_DT_DAY_OF_WEEK>(day);
        case DFTU_DT_DAY_OF_YEAR:
            return day_part<DFTU_DT_DAY_OF_YEAR>(day);
        case DFTU_DT_QUARTER:
            return day_part<DFTU_DT_QUARTER>(day);
        case DFTU_DT_IS_LEAP_YEAR:
            return day_part<DFTU_DT_IS_LEAP_YEAR>(day);
        case DFTU_DT_DAYS_IN_MONTH:
            return day_part<DFTU_DT_DAYS_IN_MONTH>(day);
        case DFTU_DT_ISO_WEEK:
            return day_part<DFTU_DT_ISO_WEEK>(day);
        case DFTU_DT_ISO_YEAR:
            return day_part<DFTU_DT_ISO_YEAR>(day);
        default:
            return day_part<DFTU_DT_EPOCH_DAYS>(day);
    }
}

// The parts of a Timestamp column in a zone: the wall time of each row. Kept
// out of line so the loop of a naive or UTC column is the one it was.
[[gnu::noinline]] bool zoned_parts(const dftu_series& v, const tz::Zone& zone,
                                   std::int64_t per_second, dftu_dt_part part,
                                   std::int64_t* out) {
    tz::OffsetCursor cursor(zone, per_second);
    for (std::int64_t i = 0; i < v.length; ++i) {
        std::int64_t wall = 0;
        if (!valid_row(v, i)) {
            out[i] = 0;
        } else if (!cursor.local(value_at(v, i, false), &wall)) {
            out_of_range();
            return false;
        } else {
            out[i] = part_of(wall, per_second, part);
        }
    }
    return true;
}

// Appends the text of row `i` of a Timestamp column in a zone; false when the
// wall time does not fit Int64.
[[gnu::noinline]] bool format_zoned(dstd::String& text, const dftu_series& v,
                                    std::int64_t i, const tz::Zone& zone,
                                    tz::OffsetCursor& cursor,
                                    std::int64_t ns_per_unit,
                                    dstd::StringView fmt) {
    const std::int64_t tick = value_at(v, i, false);
    std::int64_t wall = 0;
    if (!cursor.local(tick, &wall)) {
        out_of_range();
        return false;
    }
    const tz::Segment& seg = cursor.at(tick);
    common::format_time(text, common::civil_time(wall, ns_per_unit), fmt,
                        seg.offset, zone.types()[seg.type].abbreviation);
    return true;
}

}  // namespace
}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_dt_part(const dftu_series* in, std::int32_t part,
                                 std::int32_t unit) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_part(flat_in, part, unit));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    if (!in || part < DFTU_DT_YEAR || part > DFTU_DT_EPOCH_DAYS) return nullptr;
    if (unit < DFTU_TIME_UNIT_SECOND || unit > DFTU_TIME_UNIT_NANO)
        return nullptr;
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    TimeUnit u = TimeUnit::Micro;
    bool days = false;
    if (!resolve_unit(v, static_cast<TimeUnit>(unit), &u, &days)) {
        dftu_series_free(v);
        return nullptr;
    }
    const std::int64_t per_second = unit_per_second(u);
    const tz::Zone* zone = nullptr;
    if (!tz::column_zone(*v, &zone)) {
        dftu_series_free(v);
        return nullptr;
    }
    dftu_series* out = alloc_i64_like(v);
    auto* po = reinterpret_cast<std::int64_t*>(writable(out->data));
    const auto p = static_cast<dftu_dt_part>(part);
    if (zone) {
        if (!zoned_parts(*v, *zone, per_second, p, po)) {
            dftu_series_free(v);
            dftu_series_free(out);
            return nullptr;
        }
        dftu_series_free(v);
        return out;
    }
    fill_parts(*v, per_second, days, p, po);
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_dt_format(const dftu_series* in, const char* fmt,
                                   std::int32_t fmt_size, std::int32_t unit) {
    DFTU_FLAT_OPERAND(in, flat_in,
                      dftu_series_dt_format(flat_in, fmt, fmt_size, unit));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    if (!in || fmt_size < 0 || (fmt_size > 0 && !fmt)) return nullptr;
    if (unit < DFTU_TIME_UNIT_SECOND || unit > DFTU_TIME_UNIT_NANO)
        return nullptr;
    const dftracer::utils::dstd::StringView f(
        fmt ? fmt : "", static_cast<std::size_t>(fmt_size));
    if (!common::invalid_time_format(f).empty()) return nullptr;
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    TimeUnit u = TimeUnit::Micro;
    bool days = false;
    if (!resolve_unit(v, static_cast<TimeUnit>(unit), &u, &days)) {
        dftu_series_free(v);
        return nullptr;
    }
    const std::int64_t ns_per_unit =
        days ? common::NS_PER_DAY : common::NS_PER_SECOND / unit_per_second(u);
    const tz::Zone* zone = nullptr;
    if (!tz::column_zone(*v, &zone)) {
        dftu_series_free(v);
        return nullptr;
    }
    dstd::Vector<std::int32_t> offsets;
    offsets.reserve(static_cast<std::size_t>(v->length) + 1);
    offsets.push_back(0);
    dstd::String text;
    dstd::Optional<tz::OffsetCursor> cursor;
    if (zone) cursor.emplace(*zone, unit_per_second(u));
    for (std::int64_t i = 0; i < v->length; ++i) {
        const bool valid = !v->validity.block() ||
                           ((v->validity.data()[i >> 3] >> (i & 7)) & 1);
        if (valid && zone) {
            if (!format_zoned(text, *v, i, *zone, *cursor, ns_per_unit, f)) {
                dftu_series_free(v);
                return nullptr;
            }
        } else if (valid) {
            common::format_time(
                text, common::civil_time(value_at(*v, i, days), ns_per_unit),
                f);
        }
        if (text.size() > static_cast<std::size_t>(
                              std::numeric_limits<std::int32_t>::max())) {
            dftu_series_free(v);
            return nullptr;
        }
        offsets.push_back(static_cast<std::int32_t>(text.size()));
    }
    dftu_series* out = dftu_series_new_string(
        DFTU_TYPE_STRING, offsets.data(), text.data(), v->length,
        v->validity.block() ? v->validity.data() : nullptr);
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_dt_round(const dftu_series* in, std::int64_t every,
                                  std::int32_t mode) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_round(flat_in, every, mode));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    if (!in || every <= 0 || mode < DFTU_DT_FLOOR || mode > DFTU_DT_ROUND)
        return nullptr;
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    TimeUnit u = TimeUnit::Micro;
    bool days = false;
    if (!resolve_unit(v, TimeUnit::Micro, &u, &days) || days) {
        dftu_series_free(v);
        return nullptr;
    }
    const tz::Zone* zone = nullptr;
    if (!tz::column_zone(*v, &zone)) {
        dftu_series_free(v);
        return nullptr;
    }
    auto* out = new dftu_series();
    adopt_type_from(*out, *v);
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = new_buffer(buffer_bytes(TypeId::Int64, v->length));
    const auto* pi = reinterpret_cast<const std::int64_t*>(v->data.data());
    auto* po = reinterpret_cast<std::int64_t*>(writable(out->data));
    const auto round_tick = [&](std::int64_t t) {
        const std::int64_t lo =
            dftracer::utils::common::floor_div(t, every) * every;
        std::int64_t r = lo;
        if (mode == DFTU_DT_CEIL) {
            r = lo == t ? t : lo + every;
        } else if (mode == DFTU_DT_ROUND) {
            // Half to even, as pandas.
            const std::int64_t rem = t - lo;
            const std::int64_t twice = rem * 2;
            if (twice > every || (twice == every && (lo / every) % 2 != 0))
                r = lo + every;
        }
        return r;
    };
    if (!zone) {
        for (std::int64_t i = 0; i < v->length; ++i) po[i] = round_tick(pi[i]);
        dftu_series_free(v);
        return out;
    }
    // A zone: round the wall times, then map them back to UTC. A wall time
    // that happens twice or never is an error, as pandas.
    dstd::Vector<std::int64_t> wall;
    dstd::Vector<std::int64_t> rows;
    wall.reserve(static_cast<std::size_t>(v->length));
    rows.reserve(static_cast<std::size_t>(v->length));
    tz::OffsetCursor cursor(*zone, unit_per_second(u));
    for (std::int64_t i = 0; i < v->length; ++i) {
        po[i] = 0;
        if (!valid_row(*v, i)) continue;
        std::int64_t local = 0;
        if (!cursor.local(pi[i], &local)) {
            out_of_range();
            dftu_series_free(v);
            dftu_series_free(out);
            return nullptr;
        }
        wall.push_back(round_tick(local));
        rows.push_back(i);
    }
    dstd::Vector<std::int64_t> utc(wall.size());
    dstd::Vector<std::uint8_t> ok(wall.size());
    const auto done =
        zone->to_utc(dstd::Span<const std::int64_t>(wall.data(), wall.size()),
                     u, {}, dstd::Span<std::int64_t>(utc.data(), utc.size()),
                     dstd::Span<std::uint8_t>(ok.data(), ok.size()));
    if (!done) {
        tz::record_failure(done.error());
        dftu_series_free(v);
        dftu_series_free(out);
        return nullptr;
    }
    for (std::size_t k = 0; k < rows.size(); ++k) po[rows[k]] = utc[k];
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_dt_as_unit(const dftu_series* in, std::int32_t unit,
                                    std::int32_t mode) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_as_unit(flat_in, unit, mode));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    if (!in ||
        (in->type != TypeId::Timestamp && in->type != TypeId::Duration) ||
        unit < DFTU_TIME_UNIT_SECOND || unit > DFTU_TIME_UNIT_NANO ||
        mode < DFTU_DT_UNIT_FLOOR || mode > DFTU_DT_UNIT_POLARS)
        return nullptr;
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    const bool truncate =
        mode == DFTU_DT_UNIT_POLARS && v->type == TypeId::Duration;
    const TimeUnit from = v->time_unit();
    const TimeUnit to = static_cast<TimeUnit>(unit);
    const std::int64_t per_from = unit_per_second(from);
    const std::int64_t per_to = unit_per_second(to);
    auto* out = new dftu_series();
    adopt_type_from(*out, *v);
    out->set_time_unit(to);
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = new_buffer(buffer_bytes(TypeId::Int64, v->length));
    const auto* pi = reinterpret_cast<const std::int64_t*>(v->data.data());
    auto* po = reinterpret_cast<std::int64_t*>(writable(out->data));
    const bool up = per_to > per_from;
    const std::int64_t ratio = up ? per_to / per_from : per_from / per_to;
    const std::int64_t hi = std::numeric_limits<std::int64_t>::max() / ratio;
    const std::int64_t lo = std::numeric_limits<std::int64_t>::min() / ratio;
    const bool has_nulls = static_cast<bool>(v->validity.block());
    for (std::int64_t i = 0; i < v->length; ++i) {
        const bool valid =
            !has_nulls || ((v->validity.data()[i >> 3] >> (i & 7)) & 1);
        if (!valid) {
            po[i] = 0;
        } else if (!up) {
            po[i] = truncate ? pi[i] / ratio
                             : dftracer::utils::common::floor_div(pi[i], ratio);
        } else if (pi[i] > hi || pi[i] < lo) {
            dftu_series_free(v);
            dftu_series_free(out);
            return nullptr;
        } else {
            po[i] = pi[i] * ratio;
        }
    }
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_dt_date(const dftu_series* in) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_date(flat_in));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    if (!in || (in->type != TypeId::Timestamp && in->type != TypeId::Date64))
        return nullptr;
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    const tz::Zone* zone = nullptr;
    if (!tz::column_zone(*v, &zone)) {
        dftu_series_free(v);
        return nullptr;
    }
    const std::int64_t per_day =
        unit_per_second(v->type == TypeId::Date64 ? TimeUnit::Milli
                                                  : v->time_unit()) *
        86400;
    dstd::Optional<tz::OffsetCursor> cursor;
    if (zone) cursor.emplace(*zone, per_day / 86400);
    auto* out = new dftu_series();
    out->type = TypeId::Date32;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = new_buffer(buffer_bytes(TypeId::Date32, v->length));
    const auto* pi = reinterpret_cast<const std::int64_t*>(v->data.data());
    auto* po = reinterpret_cast<std::int32_t*>(writable(out->data));
    const bool has_nulls = static_cast<bool>(v->validity.block());
    for (std::int64_t i = 0; i < v->length; ++i) {
        const bool valid =
            !has_nulls || ((v->validity.data()[i >> 3] >> (i & 7)) & 1);
        if (!valid) {
            po[i] = 0;
            continue;
        }
        std::int64_t instant = pi[i];
        if (zone && !cursor->local(instant, &instant)) {
            out_of_range();
            dftu_series_free(v);
            dftu_series_free(out);
            return nullptr;
        }
        const std::int64_t day =
            dftracer::utils::common::floor_div(instant, per_day);
        if (day < std::numeric_limits<std::int32_t>::min() ||
            day > std::numeric_limits<std::int32_t>::max()) {
            dftu_series_free(v);
            dftu_series_free(out);
            return nullptr;
        }
        po[i] = static_cast<std::int32_t>(day);
    }
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_dt_time(const dftu_series* in) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_time(flat_in));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    if (!in || in->type != TypeId::Timestamp) return nullptr;
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    const tz::Zone* zone = nullptr;
    if (!tz::column_zone(*v, &zone)) {
        dftu_series_free(v);
        return nullptr;
    }
    const TimeUnit unit = v->time_unit();
    const std::int64_t per_day = unit_per_second(unit) * 86400;
    dstd::Optional<tz::OffsetCursor> cursor;
    if (zone) cursor.emplace(*zone, unit_per_second(unit));
    bool overflow = false;
    const auto of_day = [&](std::int64_t t) {
        if (zone && !cursor->local(t, &t)) {
            overflow = true;
            return std::int64_t{0};
        }
        return dftracer::utils::common::floor_mod(t, per_day);
    };
    const bool narrow = unit == TimeUnit::Second || unit == TimeUnit::Milli;
    auto* out = new dftu_series();
    out->type = narrow ? TypeId::Time32 : TypeId::Time64;
    out->set_time_unit(unit);
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = new_buffer(buffer_bytes(out->type, v->length));
    const auto* pi = reinterpret_cast<const std::int64_t*>(v->data.data());
    const bool has_nulls = static_cast<bool>(v->validity.block());
    auto valid_at = [&](std::int64_t i) {
        return !has_nulls || ((v->validity.data()[i >> 3] >> (i & 7)) & 1);
    };
    if (narrow) {
        auto* po = reinterpret_cast<std::int32_t*>(writable(out->data));
        for (std::int64_t i = 0; i < v->length; ++i)
            po[i] = valid_at(i) ? static_cast<std::int32_t>(of_day(pi[i])) : 0;
    } else {
        auto* po = reinterpret_cast<std::int64_t*>(writable(out->data));
        for (std::int64_t i = 0; i < v->length; ++i)
            po[i] = valid_at(i) ? of_day(pi[i]) : 0;
    }
    dftu_series_free(v);
    if (overflow) {
        out_of_range();
        dftu_series_free(out);
        return nullptr;
    }
    return out;
}

dftu_series* dftu_series_with_timezone(const dftu_series* v, const char* name,
                                       std::int32_t name_size) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_with_timezone(flat_v, name, name_size));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    tz::clear_failure();
    if (!v || v->type != TypeId::Timestamp || name_size < 0 ||
        (name_size > 0 && !name))
        return nullptr;
    const dstd::StringView zone_name(name, static_cast<std::size_t>(name_size));
    if (!zone_name.empty()) {
        auto found = tz::zone(zone_name);
        if (!found) {
            tz::record_failure(found.error());
            return nullptr;
        }
    }
    auto* out = new dftu_series(*v);
    out->set_timezone(zone_name);
    return out;
}

dftu_series* dftu_series_tz_localize(const dftu_series* v, const char* name,
                                     std::int32_t name_size,
                                     std::int32_t ambiguous,
                                     std::int32_t nonexistent,
                                     std::int64_t shift) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_tz_localize(flat_v, name, name_size,
                                              ambiguous, nonexistent, shift));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    tz::clear_failure();
    if (!v || v->type != TypeId::Timestamp || name_size < 0 ||
        (name_size > 0 && !name) || ambiguous < DFTU_TZ_AMBIGUOUS_RAISE ||
        ambiguous > DFTU_TZ_AMBIGUOUS_INFER ||
        nonexistent < DFTU_TZ_NONEXISTENT_RAISE ||
        nonexistent > DFTU_TZ_NONEXISTENT_SHIFT)
        return nullptr;
    const dstd::StringView zone_name(name, static_cast<std::size_t>(name_size));
    const TimeUnit unit = v->time_unit();
    const std::int64_t n = v->length;
    const auto* pi = reinterpret_cast<const std::int64_t*>(v->data.data());

    const tz::Zone* from = nullptr;
    if (!tz::column_zone(*v, &from)) return nullptr;
    const bool aware = !v->timezone().empty();
    if (aware) {
        if (!zone_name.empty()) {
            tz::record_failure(tz::Failure::Other,
                               "cannot localize a timestamp column that "
                               "already has a timezone");
            return nullptr;
        }
        auto* out = new dftu_series(*v);
        out->set_timezone(dstd::StringView());
        if (!from) return out;
        out->data = new_buffer(buffer_bytes(TypeId::Int64, n));
        auto* po = reinterpret_cast<std::int64_t*>(writable(out->data));
        tz::OffsetCursor cursor(*from, unit_per_second(unit));
        for (std::int64_t i = 0; i < n; ++i) {
            po[i] = 0;
            if (!valid_row(*v, i)) continue;
            if (!cursor.local(pi[i], &po[i])) {
                out_of_range();
                dftu_series_free(out);
                return nullptr;
            }
        }
        return out;
    }
    if (zone_name.empty()) return new dftu_series(*v);
    auto found = tz::zone(zone_name);
    if (!found) {
        tz::record_failure(found.error());
        return nullptr;
    }
    const tz::Zone* zone = *found;
    // A zone at UTC all the time keeps every value: only the label changes.
    std::int32_t fixed = 0;
    if (zone->fixed_offset(&fixed) && fixed == 0) {
        auto* out = new dftu_series(*v);
        out->set_timezone(zone_name);
        return out;
    }
    tz::ToUtcOptions options;
    options.ambiguous = static_cast<tz::Ambiguous>(ambiguous);
    options.nonexistent = static_cast<tz::Nonexistent>(nonexistent);
    options.shift = shift;

    dstd::Vector<std::int64_t> wall;
    dstd::Vector<std::int64_t> rows;
    wall.reserve(static_cast<std::size_t>(n));
    rows.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        if (!valid_row(*v, i)) continue;
        wall.push_back(pi[i]);
        rows.push_back(i);
    }
    dstd::Vector<std::int64_t> utc(wall.size());
    dstd::Vector<std::uint8_t> ok(wall.size());
    const auto done = zone->to_utc(
        dstd::Span<const std::int64_t>(wall.data(), wall.size()), unit, options,
        dstd::Span<std::int64_t>(utc.data(), utc.size()),
        dstd::Span<std::uint8_t>(ok.data(), ok.size()));
    if (!done) {
        tz::record_failure(done.error());
        return nullptr;
    }
    auto* out = new dftu_series(*v);
    out->set_timezone(zone_name);
    out->data = new_buffer(buffer_bytes(TypeId::Int64, n));
    auto* po = reinterpret_cast<std::int64_t*>(writable(out->data));
    std::memset(po, 0, static_cast<std::size_t>(n) * sizeof(std::int64_t));
    std::int64_t nulls = v->null_count;
    bool new_nulls = false;
    for (std::size_t k = 0; k < rows.size(); ++k) {
        po[rows[k]] = utc[k];
        if (!ok[k]) new_nulls = true;
    }
    if (new_nulls) {
        out->validity = new_buffer(buffer_bytes(TypeId::Bool, n));
        if (v->validity.block())
            std::memcpy(writable(out->validity), v->validity.data(),
                        out->validity.size());
        else
            std::memset(writable(out->validity), 0xff, out->validity.size());
        std::uint8_t* bits = writable(out->validity);
        for (std::size_t k = 0; k < rows.size(); ++k) {
            if (ok[k]) continue;
            bits[rows[k] >> 3] &=
                static_cast<std::uint8_t>(~(1u << (rows[k] & 7)));
            ++nulls;
        }
        out->null_count = nulls;
    }
    return out;
}

dftu_series* dftu_series_tz_localize_opts(const dftu_series* v,
                                          const char* name,
                                          std::int32_t name_size,
                                          std::int64_t options) {
    DFTU_FLAT_OPERAND(
        v, flat_v,
        dftu_series_tz_localize_opts(flat_v, name, name_size, options));

    using namespace dftracer::utils;
    using namespace dftracer::utils::dataframe;
    tz::clear_failure();
    if (!v || v->type != TypeId::Timestamp) return nullptr;
    const std::int32_t ambiguous = static_cast<std::int32_t>(options & 0xff);
    const std::int32_t nonexistent =
        static_cast<std::int32_t>((options >> 8) & 0xff);
    // An arithmetic shift keeps the sign of the nanosecond shift.
    const std::int64_t shift_ns = options >> 16;
    const std::int64_t ns_per_tick =
        1000000000 / unit_per_second(v->time_unit());
    if (shift_ns % ns_per_tick != 0) {
        tz::record_failure(
            tz::Failure::Other,
            dstd::fmt<"the nonexistent shift of {} ns is not a whole number "
                      "of {} ns ticks">(shift_ns, ns_per_tick));
        return nullptr;
    }
    return dftu_series_tz_localize(v, name, name_size, ambiguous, nonexistent,
                                   shift_ns / ns_per_tick);
}
