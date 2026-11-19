// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_TZ_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_TZ_H

#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <limits>

// Time zones from TZif files (RFC 8536) and POSIX TZ rules. A Zone maps a UTC
// instant to the offset in force and back from a wall time. Ticks stay UTC
// instants; a zone changes only how one reads as wall time.

namespace dftracer::utils::dataframe::tz {

/// A stretch of UTC time that has one offset. `begin` is the first UTC second
/// of the stretch and `end` the first second after it; INT64_MIN and INT64_MAX
/// mean open.
struct Segment {
    std::int64_t begin = std::numeric_limits<std::int64_t>::min();
    std::int64_t end = std::numeric_limits<std::int64_t>::max();
    /// Seconds east of UTC.
    std::int32_t offset = 0;
    /// Index into @ref Zone::types.
    std::uint32_t type = 0;
    /// Lookup hint for @ref Zone::segment_at: the number of listed transitions
    /// at or before `begin`.
    std::uint32_t index = 0;
};

/// One local time type: an offset, the DST flag and the abbreviation.
struct LocalType {
    std::int32_t offset = 0;
    bool is_dst = false;
    dstd::String abbreviation;
};

/// How a wall time resolves to UTC.
struct Resolution {
    enum class Kind : std::uint8_t {
        /// One UTC instant: `first`.
        Unique,
        /// Two UTC instants, `first` before `second`, around the clock
        /// stepping back at `transition`.
        Ambiguous,
        /// No UTC instant: the wall time is skipped by the clock stepping
        /// forward at `transition`, from `offset_before` to `offset_after`.
        Nonexistent,
    };
    Kind kind = Kind::Unique;
    std::int64_t first = 0;
    std::int64_t second = 0;
    std::int64_t transition = 0;
    std::int32_t offset_before = 0;
    std::int32_t offset_after = 0;
};

/// What @ref Zone::to_utc does with a wall time that happens twice.
enum class Ambiguous : std::uint8_t {
    Raise,
    /// The first of the two instants, the DST side (pandas `True`).
    Earliest,
    /// The second of the two instants, the standard side (pandas `False`).
    Latest,
    /// The row becomes NaT.
    Nat,
    /// As pandas `"infer"`: in each run of consecutive ambiguous rows, the
    /// rows before the wall time steps back take the first instant and the
    /// rest the second. A run needs exactly one step back.
    Infer,
};

/// What @ref Zone::to_utc does with a wall time that never happens.
enum class Nonexistent : std::uint8_t {
    Raise,
    /// The first instant after the gap (the transition itself).
    ShiftForward,
    /// One tick before the gap.
    ShiftBackward,
    /// The row becomes NaT.
    Nat,
    /// The wall time plus `ToUtcOptions::shift`, read at the offset after the
    /// gap.
    Shift,
};

struct ToUtcOptions {
    Ambiguous ambiguous = Ambiguous::Raise;
    Nonexistent nonexistent = Nonexistent::Raise;
    /// Ticks added by Nonexistent::Shift.
    std::int64_t shift = 0;
};

struct Footer;

/// An immutable time zone. Lookups are thread safe.
class Zone {
   public:
    Zone();
    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;
    Zone(Zone&&) noexcept;
    Zone& operator=(Zone&&) noexcept;
    ~Zone();

    /// The name the zone was asked for.
    dstd::StringView name() const noexcept { return name_; }
    const dstd::Vector<LocalType>& types() const noexcept { return types_; }
    /// The number of transitions listed in the file.
    std::size_t transition_count() const noexcept { return trans_.size(); }

    /// The stretch of UTC time around `utc_seconds`. `hint` is the `index` of a
    /// nearby Segment; a sorted scan that passes the previous one never
    /// searches.
    Segment segment_at(std::int64_t utc_seconds,
                       std::uint32_t hint = 0) const noexcept;

    /// Seconds east of UTC at the instant.
    std::int32_t offset_at(std::int64_t utc_seconds) const noexcept {
        return segment_at(utc_seconds).offset;
    }

    /// The offset of every tick, in `unit`, written to `out` (the same size).
    /// Rising ticks cost a comparison per row.
    void offsets_at(dstd::Span<const std::int64_t> ticks, TimeUnit unit,
                    dstd::Span<std::int32_t> out) const noexcept;

    /// The UTC instants of the wall time `local_seconds`.
    Resolution resolve_local(std::int64_t local_seconds) const noexcept;

    /// Wall times in `unit` to UTC ticks. `valid[i]` is 0 for a row that is
    /// NaT by the options, and `out[i]` is then 0. An error names the first
    /// row that is ambiguous or nonexistent under Raise, that cannot be
    /// inferred, or whose result does not fit Int64.
    dstd::Result<void> to_utc(dstd::Span<const std::int64_t> local,
                              TimeUnit unit, const ToUtcOptions& options,
                              dstd::Span<std::int64_t> out,
                              dstd::Span<std::uint8_t> valid) const;

   private:
    friend dstd::Result<Zone> parse_tzif(dstd::StringView name,
                                         dstd::Span<const std::uint8_t> bytes);
    friend dstd::Result<Zone> zone_from_posix(dstd::StringView name,
                                              dstd::StringView rule);
    friend Zone fixed_zone(dstd::StringView name, std::int32_t offset,
                           dstd::StringView abbreviation);

    Segment footer_segment(std::int64_t t) const noexcept;
    std::uint32_t table_index(std::int64_t t,
                              std::uint32_t hint) const noexcept;

    dstd::String name_;
    dstd::Vector<std::int64_t> trans_;
    dstd::Vector<std::uint8_t> trans_type_;
    dstd::Vector<LocalType> types_;
    dstd::UniquePtr<Footer> footer_;
};

/// Reads a TZif file of version 1 to 4. The 64-bit block is used when the file
/// has one, else the 32-bit block. The POSIX TZ footer of a version 2 or later
/// file gives the rules after the last listed transition.
dstd::Result<Zone> parse_tzif(dstd::StringView name,
                              dstd::Span<const std::uint8_t> bytes);

/// A zone that has no file, only a POSIX TZ rule such as
/// "EST5EDT,M3.2.0,M11.1.0" or "<+0330>-3:30".
dstd::Result<Zone> zone_from_posix(dstd::StringView name,
                                   dstd::StringView rule);

/// A zone with one offset (seconds east of UTC) for all time.
Zone fixed_zone(dstd::StringView name, std::int32_t offset,
                dstd::StringView abbreviation);

/// True when `name` is safe to join to a directory: not empty, no leading
/// separator, no empty or ".." component, only [A-Za-z0-9_+-/].
bool is_valid_zone_name(dstd::StringView name) noexcept;

/// Sets the directories searched first, separated by the platform path list
/// separator (':'). An empty list clears them. Zones already loaded stay.
void set_search_path(dstd::StringView paths);

/// The directories searched, in order: the list set by @ref set_search_path,
/// then DFTRACER_UTILS_TZDIR, then TZDIR, then the system directories.
dstd::Vector<dstd::String> search_dirs();

/// Loads `name` without the cache. A fixed offset ("+05:30", "-0800",
/// "UTC-03:00") and the UTC names ("UTC", "utc", "Z", "GMT", ...) need no
/// file. Any other name is read from the first directory that holds it: an
/// invalid name is InvalidArgument, a name in no directory is NotFound and
/// names the directories searched, a damaged file is Parse.
dstd::Result<Zone> load_zone(dstd::StringView name,
                             dstd::Span<const dstd::String> dirs);

/// The zone of `name` from the process-wide cache, loaded on first use from
/// @ref search_dirs. The Zone lives until the process shuts down.
dstd::Result<const Zone*> zone(dstd::StringView name);

}  // namespace dftracer::utils::dataframe::tz

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_TZ_H
