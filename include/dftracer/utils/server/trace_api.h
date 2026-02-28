#ifndef DFTRACER_UTILS_SERVER_TRACE_API_H
#define DFTRACER_UTILS_SERVER_TRACE_API_H

namespace dftracer::utils::server {

class Router;
class TraceIndex;

/// Register trace data API endpoints on the router:
///   GET /api/v1/files           — list available trace files
///   GET /api/v1/files/info      — file metadata (?file=...)
///   GET /api/v1/events          — query events with filters
///   GET /api/v1/events/stream   — stream events as NDJSON
///   GET /api/v1/stats           — aggregated statistics
///   GET /api/v1/info            — global time bounds and file summary
void register_trace_api(Router& router, TraceIndex& index);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_TRACE_API_H
