// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TRACE_CONFIG_H
#define DFTRACER_UTILS_TRACE_TRACE_CONFIG_H

#include <dftracer/utils/trace/args_map.h>
#include <dftracer/utils/trace/time_metric.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace {

/// One process's self-description from its "end" event. `args` holds num_events
/// plus dotted cfg.*/build.*/used.*/app.*; empty if the end carried no args.
struct TraceConfig {
    std::uint64_t pid = 0;
    ArgsMap args;
};

/// Decompress only the file's last gzip member (no index needed) and return the
/// end event(s) there, one per process, in file order. Empty when the last
/// member holds no end (reorganized trace, mid-file split chunk, crash);
/// callers should then fall back to a full scan.
std::vector<TraceConfig> read_trace_config(const std::string& trace_path);

/// The trace's native time unit, from the leading CM time_metric in the first
/// gzip member (US when absent/unreadable). Used to normalize ts/dur to a
/// target unit without decoding the whole trace.
TimeMetric read_time_metric(const std::string& trace_path);

/// Parse one NDJSON line into a TraceConfig if it is an end event; nullopt
/// otherwise. Shared by the tail probe and the index-assisted scan fallback.
std::optional<TraceConfig> parse_end_event(std::string_view line);

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_TRACE_CONFIG_H
