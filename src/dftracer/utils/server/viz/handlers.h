#ifndef DFTRACER_UTILS_SERVER_VIZ_HANDLERS_H
#define DFTRACER_UTILS_SERVER_VIZ_HANDLERS_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>

#include <string>
#include <vector>

namespace dftracer::utils::server {

class QueryParams;
class TraceIndex;

// Client viewport width (px) bounds, shared by the events and density endpoints
// to size the ~1px sub-pixel fold cutoff.
static constexpr int DEFAULT_VIEWPORT_WIDTH = 1920;
static constexpr int MIN_VIEWPORT_WIDTH = 320;
static constexpr int MAX_VIEWPORT_WIDTH = 8192;

coro::CoroTask<HttpResponse> handle_viz_events(const HttpRequest& req,
                                               const QueryParams& params,
                                               TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_stats(const HttpRequest& req,
                                              const QueryParams& params,
                                              TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_calltree(const HttpRequest& req,
                                                 const QueryParams& params,
                                                 TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_histogram(const HttpRequest& req,
                                                  const QueryParams& params,
                                                  TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_density(const HttpRequest& req,
                                                const QueryParams& params,
                                                TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_counters(const HttpRequest& req,
                                                 const QueryParams& params,
                                                 TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_proctree(const HttpRequest& req,
                                                 const QueryParams& params,
                                                 TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_layers(const HttpRequest& req,
                                               const QueryParams& params,
                                               TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_columns(const HttpRequest& req,
                                                const QueryParams& params,
                                                TraceIndex& index);
coro::CoroTask<HttpResponse> handle_viz_breaks(const HttpRequest& req,
                                               const QueryParams& params,
                                               TraceIndex& index);

// Shared across the events and density endpoints: the summary is unfiltered, so
// any server-side predicate forces a live scan (pid/tid select whole lanes and
// stay eligible); append_app_spans injects the summary's cached app spans.
bool viz_summary_eligible(const QueryParams& params);
coro::CoroTask<void> append_app_spans(std::vector<std::string>& out,
                                      TraceIndex& index, double begin,
                                      double end, const QueryParams& params);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_HANDLERS_H
