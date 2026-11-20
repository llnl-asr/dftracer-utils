// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_TZ_FOOTER_H
#define DFTRACER_UTILS_DATAFRAME_TZ_FOOTER_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::dataframe::tz {

/// A POSIX TZ date rule: `Jn`, `n` or `Mm.w.d`, with the wall time of day.
struct PosixRule {
    enum class Kind : std::uint8_t { Julian1, Julian0, MonthWeekDay };
    Kind kind = Kind::MonthWeekDay;
    std::int32_t month = 0;
    std::int32_t week = 0;
    /// Day of week (0 is Sunday) for MonthWeekDay, else the day of year.
    std::int32_t day = 0;
    /// Seconds after midnight, -167 to +167 hours.
    std::int32_t time = 7200;
};

/// The rules a TZif file ends with: standard time, and when `has_dst` the
/// yearly start and end of daylight time. `std_type` and `dst_type` index the
/// zone's local time types.
struct Footer {
    bool has_dst = false;
    std::int32_t std_offset = 0;
    std::int32_t dst_offset = 0;
    std::uint32_t std_type = 0;
    std::uint32_t dst_type = 0;
    PosixRule start;
    PosixRule end;
};

/// One change of the footer rules, in UTC seconds.
struct FooterEvent {
    std::int64_t at = 0;
    bool is_dst_start = false;
};

/// What a footer text holds before it is tied to a zone.
struct ParsedFooter {
    Footer footer;
    dstd::String std_name;
    dstd::String dst_name;
};

/// Parses "std offset [dst [offset] [,start[/time],end[/time]]]". The offset
/// signs are POSIX's (positive is west) and the result's are east.
dstd::Result<ParsedFooter> parse_posix_tz(dstd::StringView text);

/// The two changes of `year`, in time order.
void footer_events(const Footer& footer, std::int64_t year,
                   FooterEvent out[2]) noexcept;

}  // namespace dftracer::utils::dataframe::tz

#endif  // DFTRACER_UTILS_DATAFRAME_TZ_FOOTER_H
