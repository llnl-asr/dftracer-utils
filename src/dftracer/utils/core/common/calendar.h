#ifndef DFTRACER_UTILS_CORE_COMMON_CALENDAR_H
#define DFTRACER_UTILS_CORE_COMMON_CALENDAR_H

#include <dftracer/utils/core/common/int128.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// UTC calendar arithmetic shared by the dataframe kernels and the duql
// evaluator. A time is a count of units of `ns_per_unit` nanoseconds since
// 1970-01-01T00:00:00 UTC; a time before the epoch has the calendar of that
// instant, not of its absolute value.

namespace dftracer::utils {

using CalendarWide = int128_t;

constexpr std::int64_t NS_PER_SECOND = 1'000'000'000;
constexpr std::int64_t NS_PER_DAY = 86'400 * NS_PER_SECOND;

constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

constexpr std::int64_t floor_mod(std::int64_t a, std::int64_t b) {
    return a - floor_div(a, b) * b;
}

struct CivilDate {
    std::int64_t year;
    std::int32_t month;
    std::int32_t day;
};

// Howard Hinnant's civil_from_days: days since 1970-01-01 to y/m/d.
constexpr CivilDate civil_from_days(std::int64_t z) {
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
    return CivilDate{y + (m <= 2 ? 1 : 0), m, d};
}

constexpr std::int64_t days_from_civil(std::int64_t y, std::int32_t m,
                                       std::int32_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

constexpr bool is_leap_year(std::int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

constexpr std::int32_t days_in_month(std::int64_t y, std::int32_t m) {
    constexpr std::int32_t DAYS[12] = {31, 28, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31};
    return m == 2 && is_leap_year(y) ? 29 : DAYS[m - 1];
}

struct CivilTime {
    std::int64_t year;
    std::int64_t iso_year;
    std::int64_t epoch_days;
    CalendarWide epoch_seconds;
    std::int32_t month;
    std::int32_t day;
    std::int32_t hour;
    std::int32_t minute;
    std::int32_t second;
    std::int32_t nanosecond;   // within the second, 0..999999999
    std::int32_t day_of_week;  // Monday = 0
    std::int32_t day_of_year;  // 1-based
    std::int32_t iso_week;
};

// `v * ns_per_unit` is held in 128 bits, so a coarse unit or a large count
// does not overflow. `ns_per_unit` must be positive.
inline CivilTime civil_time(std::int64_t v, std::int64_t ns_per_unit) {
    const CalendarWide total = static_cast<CalendarWide>(v) * ns_per_unit;
    CalendarWide days = total / NS_PER_DAY;
    CalendarWide in_day = total - days * NS_PER_DAY;
    if (in_day < 0) {
        in_day += NS_PER_DAY;
        --days;
    }
    CivilTime t{};
    t.epoch_days = static_cast<std::int64_t>(days);
    const std::int64_t secs = static_cast<std::int64_t>(in_day / NS_PER_SECOND);
    t.nanosecond = static_cast<std::int32_t>(in_day % NS_PER_SECOND);
    t.epoch_seconds = days * 86'400 + secs;
    t.hour = static_cast<std::int32_t>(secs / 3600);
    t.minute = static_cast<std::int32_t>((secs / 60) % 60);
    t.second = static_cast<std::int32_t>(secs % 60);
    const CivilDate c = civil_from_days(t.epoch_days);
    t.year = c.year;
    t.month = c.month;
    t.day = c.day;
    t.day_of_week = static_cast<std::int32_t>(floor_mod(t.epoch_days + 3, 7));
    t.day_of_year = static_cast<std::int32_t>(t.epoch_days -
                                              days_from_civil(c.year, 1, 1)) +
                    1;
    // ISO 8601: the week holding the year's first Thursday is week 1.
    const std::int64_t thursday = t.epoch_days - t.day_of_week + 3;
    const CivilDate tc = civil_from_days(thursday);
    t.iso_year = tc.year;
    t.iso_week = static_cast<std::int32_t>(
                     (thursday - days_from_civil(tc.year, 1, 1)) / 7) +
                 1;
    return t;
}

// The codes of the dftu_dt_part values these parts share.
enum class DatePart : std::int32_t {
    Year = 0,
    Month = 1,
    Day = 2,
    Hour = 3,
    Minute = 4,
    Second = 5,
    Millisecond = 6,  // 0..999 within the second
    Microsecond = 7,  // 0..999999 within the second
    Nanosecond = 8,   // 0..999 within the microsecond
    DayOfWeek = 9,
    DayOfYear = 10,
    Quarter = 11,
    IsoWeek = 14,
    IsoYear = 15
};

constexpr bool is_date_part_code(std::int32_t code) {
    return (code >= 0 && code <= 11) || code == 14 || code == 15;
}

// The part named `name`, or false.
constexpr bool parse_date_part(std::string_view name, DatePart* out) {
    struct Entry {
        std::string_view name;
        DatePart part;
    };
    constexpr Entry TABLE[] = {
        {"year", DatePart::Year},
        {"month", DatePart::Month},
        {"day", DatePart::Day},
        {"hour", DatePart::Hour},
        {"minute", DatePart::Minute},
        {"second", DatePart::Second},
        {"millisecond", DatePart::Millisecond},
        {"microsecond", DatePart::Microsecond},
        {"nanosecond", DatePart::Nanosecond},
        {"day_of_week", DatePart::DayOfWeek},
        {"day_of_year", DatePart::DayOfYear},
        {"quarter", DatePart::Quarter},
        {"iso_week", DatePart::IsoWeek},
        {"iso_year", DatePart::IsoYear},
    };
    for (const Entry& e : TABLE) {
        if (e.name == name) {
            *out = e.part;
            return true;
        }
    }
    return false;
}

constexpr std::int64_t date_part(const CivilTime& t, DatePart part) {
    switch (part) {
        case DatePart::Year:
            return t.year;
        case DatePart::Month:
            return t.month;
        case DatePart::Day:
            return t.day;
        case DatePart::Hour:
            return t.hour;
        case DatePart::Minute:
            return t.minute;
        case DatePart::Second:
            return t.second;
        case DatePart::Millisecond:
            return t.nanosecond / 1'000'000;
        case DatePart::Microsecond:
            return t.nanosecond / 1'000;
        case DatePart::Nanosecond:
            return t.nanosecond % 1'000;
        case DatePart::DayOfWeek:
            return t.day_of_week;
        case DatePart::DayOfYear:
            return t.day_of_year;
        case DatePart::Quarter:
            return (t.month - 1) / 3 + 1;
        case DatePart::IsoWeek:
            return t.iso_week;
        case DatePart::IsoYear:
            return t.iso_year;
    }
    return 0;
}

// Empty when `fmt` holds only the supported directives (%Y %y %m %d %H %I %M
// %S %f %j %a %A %b %B %p %F %T %s %z %Z %%); otherwise the first offending
// directive, or "%" for a trailing percent sign.
inline std::string_view invalid_time_format(std::string_view fmt) {
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') continue;
        if (i + 1 == fmt.size()) return fmt.substr(i, 1);
        switch (fmt[++i]) {
            case 'Y':
            case 'y':
            case 'm':
            case 'd':
            case 'H':
            case 'I':
            case 'M':
            case 'S':
            case 'f':
            case 'j':
            case 'a':
            case 'A':
            case 'b':
            case 'B':
            case 'p':
            case 'F':
            case 'T':
            case 's':
            case 'z':
            case 'Z':
            case '%':
                break;
            default:
                return fmt.substr(i - 1, 2);
        }
    }
    return {};
}

namespace calendar_detail {

inline void append_padded(std::string& out, std::int64_t v, int width) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    for (std::ptrdiff_t n = r.ptr - buf; n < width; ++n) out.push_back('0');
    out.append(buf, r.ptr);
}

inline void append_year(std::string& out, std::int64_t y) {
    if (y < 0) {
        out.push_back('-');
        y = -y;
    }
    append_padded(out, y, 4);
}

inline void append_wide(std::string& out, CalendarWide v) {
    if (v < 0) {
        out.push_back('-');
        v = -v;
    }
    char buf[48];
    char* p = buf + sizeof buf;
    do {
        *--p = static_cast<char>('0' + static_cast<int>(v % 10));
        v /= 10;
    } while (v != 0);
    out.append(p, buf + sizeof buf);
}

constexpr std::string_view WEEKDAYS[7] = {"Monday",   "Tuesday", "Wednesday",
                                          "Thursday", "Friday",  "Saturday",
                                          "Sunday"};
constexpr std::string_view MONTHS[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

}  // namespace calendar_detail

// Appends `t` as text by `fmt`, which invalid_time_format accepted. %Y has at
// least four digits (a sign for a negative year), %f is the six-digit
// microseconds, %s the epoch seconds rounded toward minus infinity, %z is
// +0000 and %Z is UTC.
inline void format_time(std::string& out, const CivilTime& t,
                        std::string_view fmt) {
    using namespace calendar_detail;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        switch (fmt[++i]) {
            case 'Y':
                append_year(out, t.year);
                break;
            case 'y':
                append_padded(out, floor_mod(t.year, 100), 2);
                break;
            case 'm':
                append_padded(out, t.month, 2);
                break;
            case 'd':
                append_padded(out, t.day, 2);
                break;
            case 'H':
                append_padded(out, t.hour, 2);
                break;
            case 'I':
                append_padded(out, t.hour % 12 == 0 ? 12 : t.hour % 12, 2);
                break;
            case 'M':
                append_padded(out, t.minute, 2);
                break;
            case 'S':
                append_padded(out, t.second, 2);
                break;
            case 'f':
                append_padded(out, t.nanosecond / 1'000, 6);
                break;
            case 'j':
                append_padded(out, t.day_of_year, 3);
                break;
            case 'a':
                out.append(WEEKDAYS[t.day_of_week].substr(0, 3));
                break;
            case 'A':
                out.append(WEEKDAYS[t.day_of_week]);
                break;
            case 'b':
                out.append(MONTHS[t.month - 1].substr(0, 3));
                break;
            case 'B':
                out.append(MONTHS[t.month - 1]);
                break;
            case 'p':
                out.append(t.hour < 12 ? "AM" : "PM");
                break;
            case 'F':
                append_year(out, t.year);
                out.push_back('-');
                append_padded(out, t.month, 2);
                out.push_back('-');
                append_padded(out, t.day, 2);
                break;
            case 'T':
                append_padded(out, t.hour, 2);
                out.push_back(':');
                append_padded(out, t.minute, 2);
                out.push_back(':');
                append_padded(out, t.second, 2);
                break;
            case 's':
                append_wide(out, t.epoch_seconds);
                break;
            case 'z':
                out.append("+0000");
                break;
            case 'Z':
                out.append("UTC");
                break;
            default:
                out.push_back('%');
                break;
        }
    }
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_CALENDAR_H
