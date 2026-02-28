#ifndef DFTRACER_UTILS_SERVER_VIZ_API_H
#define DFTRACER_UTILS_SERVER_VIZ_API_H

namespace dftracer::utils::server {

class Router;
class TraceIndex;

/// Register visualization API endpoints on the router:
///   GET /api/v1/viz/events — query events for visualization with
///       time-range windowing, lane grouping, and summary aggregation
void register_viz_api(Router& router, TraceIndex& index);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_API_H
