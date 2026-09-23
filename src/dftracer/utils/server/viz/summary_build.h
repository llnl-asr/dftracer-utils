#ifndef DFTRACER_UTILS_SERVER_VIZ_SUMMARY_BUILD_H
#define DFTRACER_UTILS_SERVER_VIZ_SUMMARY_BUILD_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_summary.h>

namespace dftracer::utils::server {

// Return the trace's activity summary (the mipmap over the whole trace),
// building it on first use. Concurrent callers wait for the in-flight build.
coro::CoroTask<const VizSummary*> ensure_viz_summary(TraceIndex& index);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_SUMMARY_BUILD_H
