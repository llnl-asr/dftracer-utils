#ifndef DFTRACER_UTILS_SERVER_VIZ_API_H
#define DFTRACER_UTILS_SERVER_VIZ_API_H

#include <dftracer/utils/core/coro/task.h>

namespace dftracer::utils::server {

class Router;
class TraceIndex;

/// Register visualization API endpoints on the router:
///   GET /api/v1/viz/events - query events for visualization with
///       time-range windowing, lane grouping, and summary aggregation
void register_viz_api(Router& router, TraceIndex& index);

/// Build the activity summary if not already present (idempotent). Run at
/// startup so the first heavy request does not pay the full-scan build cost.
coro::CoroTask<void> prewarm_viz_summary(TraceIndex& index);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_API_H
