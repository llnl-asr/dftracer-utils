// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_SERVER_ROUTER_H
#define DFTRACER_UTILS_DOMAINS_SERVER_ROUTER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <utility>

namespace dftracer::utils::domains::server {

struct HttpRequest;
struct HttpResponse;

/// Simple URL query parameter map.
class QueryParams {
   public:
    /// Parse from a query string ("key=value&key2=value2").
    static QueryParams parse(dstd::StringView query);

    dstd::StringView get(dstd::StringView key,
                         dstd::StringView default_value = {}) const;
    bool has(dstd::StringView key) const;
    int get_int(dstd::StringView key, int default_value = 0) const;
    double get_double(dstd::StringView key, double default_value = 0) const;

    /// Order-independent signature of all params, for use as a result-cache
    /// key.
    dstd::String canonical_key() const;

   private:
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> params_;
};

/// Handler function type for routes.
using RouteHandler = dstd::Function<coro::CoroTask<HttpResponse>(
    const HttpRequest&, const QueryParams&)>;

/// One query parameter of a route, for the generated API docs / OpenAPI spec.
struct RouteParam {
    dstd::String name;
    dstd::String desc;
    bool required = false;
    dstd::String example;  ///< default / example value shown in the explorer
};

/// Optional documentation attached to a route. When `summary` is empty the
/// route is treated as internal and left out of the generated docs.
struct RouteDoc {
    dstd::String summary;
    dstd::String tag;  ///< grouping, e.g. "Trace data" / "Visualization"
    dstd::Vector<RouteParam> params;
    dstd::String response_example;  ///< representative JSON response
};

/// A registered route plus its docs.
struct Route {
    dstd::String method;
    dstd::String path;
    RouteHandler handler;
    RouteDoc doc;
};

/// Simple prefix-based HTTP router.
class Router {
   public:
    void get(const dstd::String& path, RouteHandler handler);
    void get(const dstd::String& path, RouteHandler handler, RouteDoc doc);
    void post(const dstd::String& path, RouteHandler handler);
    void post(const dstd::String& path, RouteHandler handler, RouteDoc doc);

    /// Registered routes in registration order (for API-docs generation).
    const dstd::Vector<Route>& routes() const { return routes_; }

    /// When non-empty, every request must present this token (via a ?token=
    /// query param or an "Authorization: Bearer <token>" header) or gets 401.
    void set_auth_token(dstd::String token) { auth_token_ = std::move(token); }

    /// Match request method + path, parse query params, invoke handler.
    /// Returns 404 if no route matches.
    coro::CoroTask<HttpResponse> handle(const HttpRequest& req);

   private:
    dstd::Vector<Route> routes_;
    dstd::String auth_token_;
};

}  // namespace dftracer::utils::domains::server

#endif  // DFTRACER_UTILS_DOMAINS_SERVER_ROUTER_H
