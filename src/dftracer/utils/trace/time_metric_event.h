// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TIME_METRIC_EVENT_H
#define DFTRACER_UTILS_TRACE_TIME_METRIC_EVENT_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/time_metric.h>

namespace dftracer::utils::trace {

/// True when the event is the CM time_metric declaration; sets `out` to its
/// unit.
inline bool extract_time_metric(const DFTracerEvent& event, TimeMetric& out) {
    if (!event.is_metadata() || event.name != "CM") return false;
    auto name = event.args["name"];
    if (!name.is_string() || name.get<dstd::StringView>() != "time_metric")
        return false;
    auto value = event.args["value"];
    if (!value.is_string()) return false;
    out = parse_time_metric(value.get<dstd::StringView>());
    return true;
}

/// The trace's native time unit, from the leading CM time_metric in the first
/// gzip member (US when absent/unreadable). Used to normalize ts/dur to a
/// target unit without decoding the whole trace.
TimeMetric read_time_metric(const dstd::String& trace_path);

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_TIME_METRIC_EVENT_H
