// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TRACE_CONFIG_H
#define DFTRACER_UTILS_TRACE_TRACE_CONFIG_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/args_map.h>

#include <cstdint>

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
dstd::Vector<TraceConfig> read_trace_config(const dstd::String& trace_path);

/// Parse one NDJSON line into a @ref TraceConfig if it is an end event; nullopt
/// otherwise. Shared by the tail probe and the index-assisted scan fallback.
dstd::Optional<TraceConfig> parse_end_event(dstd::StringView line);

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_TRACE_CONFIG_H
