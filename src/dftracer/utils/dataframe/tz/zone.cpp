// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/calendar.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dataframe/tz/footer.h>
#include <dftracer/utils/dstd/int128.h>

#include <algorithm>

namespace dftracer::utils::dataframe::tz {

namespace {

constexpr std::int64_t MIN_TIME = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t MAX_TIME = std::numeric_limits<std::int64_t>::max();

// A wall time lies within this many seconds of the UTC instants it names.
constexpr std::int64_t WINDOW = 100'000;
// Past this the footer rules are not evaluated: the year would overflow.
constexpr std::int64_t FOOTER_LIMIT = std::int64_t{1} << 58;

std::int64_t seconds_per_tick(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::Second:
            return 1;
        case TimeUnit::Milli:
            return 1'000;
        case TimeUnit::Micro:
            return 1'000'000;
        case TimeUnit::Nano:
            return 1'000'000'000;
    }
    return 1;
}

// Seconds to ticks, saturating at the Int64 limits.
std::int64_t to_ticks(std::int64_t seconds, std::int64_t per) {
    if (seconds <= MIN_TIME / per) return MIN_TIME;
    if (seconds >= MAX_TIME / per) return MAX_TIME;
    return seconds * per;
}

bool fits(dstd::int128_t v, std::int64_t* out) {
    if (v < MIN_TIME || v > MAX_TIME) return false;
    *out = static_cast<std::int64_t>(v);
    return true;
}

std::int64_t clamp_wide(dstd::int128_t v) {
    if (v < MIN_TIME) return MIN_TIME;
    if (v > MAX_TIME) return MAX_TIME;
    return static_cast<std::int64_t>(v);
}

std::int64_t saturating_sub(std::int64_t a, std::int64_t b) {
    return clamp_wide(static_cast<dstd::int128_t>(a) - b);
}

std::int64_t saturating_add(std::int64_t a, std::int64_t b) {
    return clamp_wide(static_cast<dstd::int128_t>(a) + b);
}

// The part of `v` below one second, in ticks, for floor semantics.
std::int64_t tick_remainder(std::int64_t v, std::int64_t per) {
    const std::int64_t r = v % per;
    return r < 0 ? r + per : r;
}

}  // namespace

Zone::Zone() = default;
Zone::Zone(Zone&&) noexcept = default;
Zone& Zone::operator=(Zone&&) noexcept = default;
Zone::~Zone() = default;

std::uint32_t Zone::table_index(std::int64_t t,
                                std::uint32_t hint) const noexcept {
    const std::size_t n = trans_.size();
    std::size_t h = hint < n ? hint : n;
    const std::int64_t* first = trans_.data();
    if (h < n && first[h] <= t) {
        if (h + 1 == n || t < first[h + 1])
            return static_cast<std::uint32_t>(h + 1);
        return static_cast<std::uint32_t>(
            std::upper_bound(first + h + 1, first + n, t) - first);
    }
    if (h == 0 || first[h - 1] <= t) return static_cast<std::uint32_t>(h);
    return static_cast<std::uint32_t>(
        std::upper_bound(first, first + h - 1, t) - first);
}

Segment Zone::footer_segment(std::int64_t t) const noexcept {
    const Footer& f = *footer_;
    Segment s;
    s.index = static_cast<std::uint32_t>(trans_.size());
    s.type = f.std_type;
    s.offset = f.std_offset;
    if (f.has_dst && t > -FOOTER_LIMIT && t < FOOTER_LIMIT) {
        std::int64_t year =
            common::civil_from_days(common::floor_div(t + f.std_offset, 86400))
                .year;
        const auto type_of = [&](bool is_dst_start) {
            return is_dst_start ? f.dst_type : f.std_type;
        };
        // The year whose first change is at or before t and whose next year's
        // first change is after it.
        FooterEvent cur[2];
        FooterEvent next[2];
        footer_events(f, year, cur);
        for (;;) {
            if (t < cur[0].at) {
                footer_events(f, --year, cur);
                continue;
            }
            if (t < cur[1].at) {
                s.begin = cur[0].at;
                s.end = cur[1].at;
                s.type = type_of(cur[0].is_dst_start);
                break;
            }
            footer_events(f, year + 1, next);
            if (t >= next[0].at) {
                ++year;
                cur[0] = next[0];
                cur[1] = next[1];
                continue;
            }
            s.begin = cur[1].at;
            s.end = next[0].at;
            s.type = type_of(cur[1].is_dst_start);
            break;
        }
        s.offset = s.type == f.dst_type ? f.dst_offset : f.std_offset;
    }
    if (!trans_.empty() && s.begin < trans_.back()) s.begin = trans_.back();
    return s;
}

Segment Zone::segment_at(std::int64_t t, std::uint32_t hint) const noexcept {
    const std::size_t n = trans_.size();
    if (footer_ && (n == 0 || t >= trans_[n - 1])) return footer_segment(t);
    Segment s;
    if (n == 0) {
        s.offset = types_[0].offset;
        return s;
    }
    const std::uint32_t i = table_index(t, hint);
    s.index = i;
    if (i == 0) {
        s.end = trans_[0];
        s.type = 0;  // RFC 8536 section 3.2: type 0 holds before the first
                     // transition
    } else {
        s.begin = trans_[i - 1];
        if (i < n) s.end = trans_[i];
        s.type = trans_type_[i - 1];
    }
    s.offset = types_[s.type].offset;
    return s;
}

void Zone::offsets_at(dstd::Span<const std::int64_t> ticks, TimeUnit unit,
                      dstd::Span<std::int32_t> out) const noexcept {
    const std::int64_t per = seconds_per_tick(unit);
    std::int64_t lo = 1;
    std::int64_t hi = 0;
    std::int32_t offset = 0;
    std::uint32_t hint = 0;
    const std::size_t count = ticks.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t v = ticks[i];
        if (v < lo || v >= hi) {
            const Segment s = segment_at(common::floor_div(v, per), hint);
            hint = s.index;
            lo = s.begin == MIN_TIME ? MIN_TIME : to_ticks(s.begin, per);
            hi = to_ticks(s.end, per);
            offset = s.offset;
        }
        out[i] = offset;
    }
}

Resolution Zone::resolve_local(std::int64_t w) const noexcept {
    std::int64_t candidates[8];
    std::size_t found = 0;
    Resolution gap;
    Resolution overlap;
    bool has_gap = false;
    bool has_overlap = false;

    Segment s = segment_at(saturating_sub(w, WINDOW));
    Segment prev;
    bool has_prev = false;
    const std::int64_t stop = saturating_add(w, WINDOW);
    // Wall times near the Int64 limits and a sentinel transition at the lowest
    // time would wrap in 64 bits.
    const dstd::int128_t wide = w;
    for (;;) {
        const dstd::int128_t u = wide - s.offset;
        if (u >= s.begin && u <= MAX_TIME && (u < s.end || s.end == MAX_TIME) &&
            found < 8)
            candidates[found++] = static_cast<std::int64_t>(u);
        if (has_prev) {
            const dstd::int128_t t = s.begin;
            if (s.offset > prev.offset && wide >= t + prev.offset &&
                wide < t + s.offset && !has_gap) {
                has_gap = true;
                gap.kind = Resolution::Kind::Nonexistent;
                gap.transition = s.begin;
                gap.offset_before = prev.offset;
                gap.offset_after = s.offset;
            }
            if (s.offset < prev.offset && wide >= t + s.offset &&
                wide < t + prev.offset && !has_overlap) {
                has_overlap = true;
                overlap.kind = Resolution::Kind::Ambiguous;
                overlap.transition = s.begin;
                overlap.offset_before = prev.offset;
                overlap.offset_after = s.offset;
            }
        }
        if (s.end == MAX_TIME || s.end > stop) break;
        prev = s;
        has_prev = true;
        s = segment_at(s.end, s.index);
    }

    Resolution r;
    if (found == 1) {
        r.first = r.second = candidates[0];
        return r;
    }
    if (found == 0) {
        if (has_gap) return gap;
        r.kind = Resolution::Kind::Nonexistent;
        r.transition = w;
        return r;
    }
    std::sort(candidates, candidates + found);
    r = has_overlap ? overlap : Resolution{};
    r.kind = Resolution::Kind::Ambiguous;
    r.first = candidates[0];
    r.second = candidates[found - 1];
    return r;
}

namespace {

// The wall times, in ticks, that name one instant of a segment and no other,
// and the segment's offset in ticks. An empty range means the segment is not
// cached.
struct UniqueRange {
    std::int64_t lo = 1;
    std::int64_t hi = 0;
    std::int64_t offset = 0;
};

// A segment this short could share wall times with the one after its
// neighbour, so no range is kept near it.
bool is_long(const Segment& s) {
    return s.begin == MIN_TIME || s.end == MAX_TIME ||
           static_cast<dstd::int128_t>(s.end) - s.begin > 2 * WINDOW;
}

UniqueRange unique_range(const Zone& zone, const Segment& s, std::int64_t per,
                         std::int64_t margin) {
    Segment before = s;
    Segment after = s;
    if (s.begin != MIN_TIME) before = zone.segment_at(s.begin - 1);
    if (s.end != MAX_TIME) after = zone.segment_at(s.end);
    UniqueRange range;
    if (!is_long(s) || !is_long(before) || !is_long(after)) return range;
    range.lo =
        s.begin == MIN_TIME
            ? MIN_TIME + margin
            : std::max(MIN_TIME + margin,
                       to_ticks(saturating_add(
                                    s.begin, std::max(s.offset, before.offset)),
                                per));
    range.hi =
        s.end == MAX_TIME
            ? MAX_TIME - margin
            : std::min(MAX_TIME - margin,
                       to_ticks(saturating_add(
                                    s.end, std::min(s.offset, after.offset)),
                                per));
    range.offset = static_cast<std::int64_t>(s.offset) * per;
    return range;
}

}  // namespace

dstd::Result<void> Zone::to_utc(dstd::Span<const std::int64_t> local,
                                TimeUnit unit, const ToUtcOptions& options,
                                dstd::Span<std::int64_t> out,
                                dstd::Span<std::uint8_t> valid) const {
    const std::int64_t per = seconds_per_tick(unit);
    const std::int64_t margin = WINDOW * per;
    UniqueRange cache;

    struct Run {
        bool in_run = false;
        bool stepped_back = false;
        std::int64_t previous = 0;
        std::size_t row = 0;
    } run;

    const auto fail = [](dstd::StringView what, std::size_t row) {
        return common::make_error<"{} wall time at row {}">(
            common::ErrorCode::InvalidArgument, what, row);
    };
    const auto close_run = [&]() -> dstd::Result<void> {
        const bool unresolved = run.in_run && !run.stepped_back;
        run.in_run = false;
        if (unresolved)
            return common::make_error<
                "cannot infer the ambiguous wall time at row {}: the run has "
                "no repeated times">(common::ErrorCode::InvalidArgument,
                                     run.row);
        return {};
    };
    const auto out_of_range = [](std::size_t row) {
        return common::make_error<"wall time at row {} is out of range">(
            common::ErrorCode::InvalidArgument, row);
    };

    const std::size_t count = local.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t w = local[i];
        if (w >= cache.lo && w < cache.hi) {
            if (run.in_run) {
                auto closed = close_run();
                if (!closed) return closed;
            }
            out[i] = w - cache.offset;
            valid[i] = 1;
            continue;
        }

        const std::int64_t seconds = common::floor_div(w, per);
        const std::int64_t rem = tick_remainder(w, per);
        const Resolution r = resolve_local(seconds);
        std::int64_t result = 0;
        bool has_value = true;

        if (r.kind == Resolution::Kind::Unique) {
            if (run.in_run) {
                auto closed = close_run();
                if (!closed) return closed;
            }
            if (!fits(static_cast<dstd::int128_t>(r.first) * per + rem,
                      &result))
                return out_of_range(i);
            cache = unique_range(*this, segment_at(r.first), per, margin);
        } else if (r.kind == Resolution::Kind::Ambiguous) {
            std::int64_t instant = r.first;
            switch (options.ambiguous) {
                case Ambiguous::Raise:
                    return fail("ambiguous", i);
                case Ambiguous::Earliest:
                    break;
                case Ambiguous::Latest:
                    instant = r.second;
                    break;
                case Ambiguous::Nat:
                    has_value = false;
                    break;
                case Ambiguous::Infer:
                    if (run.in_run) {
                        if (w <= run.previous) {
                            if (run.stepped_back)
                                return common::make_error<
                                    "cannot infer the ambiguous wall time at "
                                    "row {}: more than one switch">(
                                    common::ErrorCode::InvalidArgument, i);
                            run.stepped_back = true;
                        }
                    } else {
                        run = Run{true, false, w, i};
                    }
                    run.previous = w;
                    if (run.stepped_back) instant = r.second;
                    break;
            }
            if (has_value &&
                !fits(static_cast<dstd::int128_t>(instant) * per + rem,
                      &result))
                return out_of_range(i);
        } else {
            // No gap at all: the instant of this wall time does not fit Int64.
            if (r.offset_before == r.offset_after) return out_of_range(i);
            if (run.in_run) {
                auto closed = close_run();
                if (!closed) return closed;
            }
            switch (options.nonexistent) {
                case Nonexistent::Raise:
                    return fail("nonexistent", i);
                case Nonexistent::Nat:
                    has_value = false;
                    break;
                case Nonexistent::ShiftForward:
                    if (!fits(static_cast<dstd::int128_t>(r.transition) * per,
                              &result))
                        return out_of_range(i);
                    break;
                case Nonexistent::ShiftBackward:
                    if (!fits(
                            static_cast<dstd::int128_t>(r.transition) * per - 1,
                            &result))
                        return out_of_range(i);
                    break;
                case Nonexistent::Shift: {
                    std::int64_t moved = 0;
                    if (!fits(static_cast<dstd::int128_t>(w) + options.shift,
                              &moved))
                        return out_of_range(i);
                    const std::int64_t moved_seconds =
                        common::floor_div(moved, per);
                    const Resolution after = resolve_local(moved_seconds);
                    if (after.kind != Resolution::Kind::Unique)
                        return fail("nonexistent", i);
                    if (!fits(static_cast<dstd::int128_t>(after.first) * per +
                                  tick_remainder(moved, per),
                              &result))
                        return out_of_range(i);
                    break;
                }
            }
        }
        out[i] = has_value ? result : 0;
        valid[i] = has_value ? 1 : 0;
    }
    return close_run();
}

}  // namespace dftracer::utils::dataframe::tz
