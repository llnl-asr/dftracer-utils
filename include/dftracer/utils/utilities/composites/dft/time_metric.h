#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_TIME_METRIC_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_TIME_METRIC_H

#include <dftracer/utils/utilities/composites/dft/event.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft {

// Time unit a trace records ts/dur in, declared via the CM metadata event
// {"name":"CM","ph":"M","args":{"name":"time_metric","value":"NS"}}.
// Microseconds (US) is the canonical unit; an absent or unknown metric is US.
enum class TimeMetric { US, NS, MS, SEC };

inline TimeMetric parse_time_metric(std::string_view value) {
    if (value == "NS") return TimeMetric::NS;
    if (value == "MS") return TimeMetric::MS;
    if (value == "SEC") return TimeMetric::SEC;
    return TimeMetric::US;
}

inline std::string_view time_metric_to_string(TimeMetric metric) {
    switch (metric) {
        case TimeMetric::NS:
            return "NS";
        case TimeMetric::MS:
            return "MS";
        case TimeMetric::SEC:
            return "SEC";
        case TimeMetric::US:
        default:
            return "US";
    }
}

inline std::uint64_t time_metric_ns_per_unit(TimeMetric metric) {
    switch (metric) {
        case TimeMetric::NS:
            return 1ULL;
        case TimeMetric::US:
            return 1000ULL;
        case TimeMetric::MS:
            return 1000000ULL;
        case TimeMetric::SEC:
            return 1000000000ULL;
        default:
            return 1000ULL;
    }
}

// Convert a value from unit `from` to unit `to`. Units are exact powers of ten,
// so one factor divides the other; a downscale (e.g. NS->US) truncates.
inline std::uint64_t scale_between(TimeMetric from, TimeMetric to,
                                   std::uint64_t value) {
    if (from == to) return value;
    std::uint64_t nf = time_metric_ns_per_unit(from);
    std::uint64_t nt = time_metric_ns_per_unit(to);
    return nf >= nt ? value * (nf / nt) : value / (nt / nf);
}

inline std::uint64_t scale_to_us(TimeMetric metric, std::uint64_t value) {
    return scale_between(metric, TimeMetric::US, value);
}

// Multiplier that converts a native value to microseconds, for fractional
// durations/thresholds. US -> 1.0, NS -> 0.001, MS -> 1000.0, SEC -> 1e6.
inline double time_metric_us_scale(TimeMetric metric) {
    return static_cast<double>(time_metric_ns_per_unit(metric)) / 1000.0;
}

// Per-file time-unit state carried across rows on a read: `metric` is the
// file's native unit (from its CM declaration); `target`, when set, requests
// scaling ts/dur from `metric` into that unit.
struct TimeScaleState {
    TimeMetric metric = TimeMetric::US;
    std::optional<TimeMetric> target;
};

// True when the event is the CM time_metric declaration; sets `out` to its
// unit.
inline bool extract_time_metric(const DFTracerEvent& event, TimeMetric& out) {
    if (!event.is_metadata() || event.name != "CM") return false;
    auto name = event.args["name"];
    if (!name.is_string() || name.get<std::string_view>() != "time_metric")
        return false;
    auto value = event.args["value"];
    if (!value.is_string()) return false;
    out = parse_time_metric(value.get<std::string_view>());
    return true;
}

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_TIME_METRIC_H
