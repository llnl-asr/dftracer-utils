#ifndef DFTRACER_UTILS_SERVER_ROUTER_H
#define DFTRACER_UTILS_SERVER_ROUTER_H

#include <dftracer/utils/core/coro/task.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::server {

struct HttpRequest;
struct HttpResponse;

/// Simple URL query parameter map.
class QueryParams {
   public:
    /// Parse from a query string ("key=value&key2=value2").
    static QueryParams parse(std::string_view query);

    std::string_view get(std::string_view key,
                         std::string_view default_value = {}) const;
    bool has(std::string_view key) const;
    int get_int(std::string_view key, int default_value = 0) const;
    double get_double(std::string_view key, double default_value = 0) const;

    /// Order-independent signature of all params, for use as a result-cache
    /// key.
    std::string canonical_key() const;

   private:
    std::vector<std::pair<std::string, std::string>> params_;
};

/// Handler function type for routes.
using RouteHandler = std::function<coro::CoroTask<HttpResponse>(
    const HttpRequest&, const QueryParams&)>;

/// One query parameter of a route, for the generated API docs / OpenAPI spec.
struct RouteParam {
    std::string name;
    std::string desc;
    bool required = false;
    std::string example;  // default / example value shown in the explorer
};

/// Optional documentation attached to a route. When `summary` is empty the
/// route is treated as internal and left out of the generated docs.
struct RouteDoc {
    std::string summary;
    std::string tag;  // grouping, e.g. "Trace data" / "Visualization"
    std::vector<RouteParam> params;
    std::string response_example;  // representative JSON response
};

/// A registered route plus its docs.
struct Route {
    std::string method;
    std::string path;
    RouteHandler handler;
    RouteDoc doc;
};

/// Simple prefix-based HTTP router.
class Router {
   public:
    void get(const std::string& path, RouteHandler handler);
    void get(const std::string& path, RouteHandler handler, RouteDoc doc);
    void post(const std::string& path, RouteHandler handler);
    void post(const std::string& path, RouteHandler handler, RouteDoc doc);

    /// Registered routes in registration order (for API-docs generation).
    const std::vector<Route>& routes() const { return routes_; }

    /// When non-empty, every request must present this token (via a ?token=
    /// query param or an "Authorization: Bearer <token>" header) or gets 401.
    void set_auth_token(std::string token) { auth_token_ = std::move(token); }

    /// Match request method + path, parse query params, invoke handler.
    /// Returns 404 if no route matches.
    coro::CoroTask<HttpResponse> handle(const HttpRequest& req);

   private:
    std::vector<Route> routes_;
    std::string auth_token_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_ROUTER_H
