#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <cstring>

// The calendar parts of an instant (the pandas .dt accessor) and rounding to a
// bucket, over a Timestamp / Date32 / Date64 / Duration column (its own unit)
// or a plain Int64 column read in a given unit. Naive: a timezone on the
// column is ignored and every part is in UTC.

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

// Floor division and remainder toward minus infinity (an instant before the
// epoch still lands in its own day).
std::int64_t fdiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
std::int64_t fmod_(std::int64_t a, std::int64_t b) {
    return a - fdiv(a, b) * b;
}

struct Civil {
    std::int64_t year;
    std::int32_t month;
    std::int32_t day;
};

// Howard Hinnant's days-from-civil inverse: days since 1970-01-01 to y/m/d.
Civil civil_from_days(std::int64_t z) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int32_t d =
        static_cast<std::int32_t>(doy - (153 * mp + 2) / 5 + 1);
    const std::int32_t m = static_cast<std::int32_t>(mp < 10 ? mp + 3 : mp - 9);
    return Civil{y + (m <= 2 ? 1 : 0), m, d};
}

std::int64_t days_from_civil(std::int64_t y, std::int32_t m, std::int32_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool leap(std::int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

std::int32_t days_in_month(std::int64_t y, std::int32_t m) {
    static const std::int32_t days[12] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : days[m - 1];
}

// The unit an input column's values are in: its own for the temporal types,
// `fallback` for an Int64, and days for the dates.
bool resolve_unit(const dftu_series* v, TimeUnit fallback, TimeUnit* unit,
                  bool* days) {
    *days = false;
    switch (v->type) {
        case TypeId::Timestamp:
        case TypeId::Duration:
            *unit = v->time_unit;
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
    if (days) return reinterpret_cast<const std::int32_t*>(v.data->data())[i];
    return reinterpret_cast<const std::int64_t*>(v.data->data())[i];
}

dftu_series* alloc_i64_like(const dftu_series* v) {
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = Buffer::allocate(buffer_bytes(TypeId::Int64, v->length));
    return out;
}

std::int64_t part_of(std::int64_t t, std::int64_t per_second, bool days,
                     dftu_dt_part part) {
    const std::int64_t day_len = days ? 1 : per_second * 86400;
    const std::int64_t day = fdiv(t, day_len);
    const std::int64_t in_day = days ? 0 : fmod_(t, day_len);
    const std::int64_t secs = days ? 0 : in_day / per_second;
    const std::int64_t frac = days ? 0 : in_day % per_second;
    const Civil c = civil_from_days(day);
    switch (part) {
        case DFTU_DT_YEAR:
            return c.year;
        case DFTU_DT_MONTH:
            return c.month;
        case DFTU_DT_DAY:
            return c.day;
        case DFTU_DT_HOUR:
            return secs / 3600;
        case DFTU_DT_MINUTE:
            return (secs / 60) % 60;
        case DFTU_DT_SECOND:
            return secs % 60;
        case DFTU_DT_MILLISECOND:
            return frac * 1'000 / per_second;
        case DFTU_DT_MICROSECOND:
            return frac * 1'000'000 / per_second;
        case DFTU_DT_NANOSECOND:
            return per_second >= 1'000'000'000
                       ? frac % 1'000
                       : frac * (1'000'000'000 / per_second) % 1'000;
        case DFTU_DT_DAY_OF_WEEK:
            // 1970-01-01 was a Thursday (3, Monday = 0).
            return fmod_(day + 3, 7);
        case DFTU_DT_DAY_OF_YEAR:
            return day - days_from_civil(c.year, 1, 1) + 1;
        case DFTU_DT_QUARTER:
            return (c.month - 1) / 3 + 1;
        case DFTU_DT_IS_LEAP_YEAR:
            return leap(c.year) ? 1 : 0;
        case DFTU_DT_DAYS_IN_MONTH:
            return days_in_month(c.year, c.month);
        case DFTU_DT_ISO_WEEK:
        case DFTU_DT_ISO_YEAR: {
            // ISO 8601: the week holding the year's first Thursday is week 1.
            const std::int64_t wd = fmod_(day + 3, 7);  // Monday = 0
            const std::int64_t thursday = day - wd + 3;
            const Civil tc = civil_from_days(thursday);
            if (part == DFTU_DT_ISO_YEAR) return tc.year;
            const std::int64_t jan1 = days_from_civil(tc.year, 1, 1);
            return (thursday - jan1) / 7 + 1;
        }
        case DFTU_DT_EPOCH_DAYS:
            return day;
    }
    return 0;
}

}  // namespace
}  // namespace dftracer::utils::dataframe

extern "C" {

dftu_series* dftu_series_dt_part(const dftu_series* in, int32_t part,
                                 int32_t unit) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_part(flat_in, part, unit));

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
    dftu_series* out = alloc_i64_like(v);
    auto* po = reinterpret_cast<std::int64_t*>(out->data->data());
    const auto p = static_cast<dftu_dt_part>(part);
    for (std::int64_t i = 0; i < v->length; ++i)
        po[i] = part_of(value_at(*v, i, days), per_second, days, p);
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_dt_round(const dftu_series* in, int64_t every,
                                  int32_t mode) {
    DFTU_FLAT_OPERAND(in, flat_in, dftu_series_dt_round(flat_in, every, mode));

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
    auto* out = new dftu_series();
    adopt_type_from(*out, *v);
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = Buffer::allocate(buffer_bytes(TypeId::Int64, v->length));
    const auto* pi = reinterpret_cast<const std::int64_t*>(v->data->data());
    auto* po = reinterpret_cast<std::int64_t*>(out->data->data());
    for (std::int64_t i = 0; i < v->length; ++i) {
        const std::int64_t t = pi[i];
        const std::int64_t lo = fdiv(t, every) * every;
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
        po[i] = r;
    }
    dftu_series_free(v);
    return out;
}

dftu_series* dftu_series_with_timezone(const dftu_series* v, const char* tz,
                                       int32_t tz_len) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_with_timezone(flat_v, tz, tz_len));

    using namespace dftracer::utils::dataframe;
    if (!v || v->type != TypeId::Timestamp || tz_len < 0 || (tz_len > 0 && !tz))
        return nullptr;
    auto* out = new dftu_series(*v);
    out->timezone.assign(tz, static_cast<std::size_t>(tz_len));
    return out;
}

}  // extern "C"
