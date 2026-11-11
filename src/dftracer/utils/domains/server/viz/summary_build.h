// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_SERVER_VIZ_SUMMARY_BUILD_H
#define DFTRACER_UTILS_DOMAINS_SERVER_VIZ_SUMMARY_BUILD_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz_summary.h>
#include <dftracer/utils/dstd/result.h>

namespace dftracer::utils::domains::server {

// Return the trace's activity summary (the mipmap over the whole trace),
// building it on first use. Concurrent callers wait for the in-flight build.
coro::CoroTask<dstd::Result<const VizSummary*>> ensure_viz_summary(
    TraceIndex& index);

}  // namespace dftracer::utils::domains::server

#endif  // DFTRACER_UTILS_DOMAINS_SERVER_VIZ_SUMMARY_BUILD_H
