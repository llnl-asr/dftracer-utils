#ifndef DFTRACER_UTILS_SERVER_ROUTER_H
#define DFTRACER_UTILS_SERVER_ROUTER_H

#include <dftracer/utils/core/coro/task.h>

#include <functional>
#include <string>
#include <string_view>
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

   private:
    std::vector<std::pair<std::string, std::string>> params_;
};

/// Handler function type for routes.
using RouteHandler = std::function<coro::CoroTask<HttpResponse>(
    const HttpRequest&, const QueryParams&)>;

/// Simple prefix-based HTTP router.
class Router {
   public:
    void get(const std::string& path, RouteHandler handler);
    void post(const std::string& path, RouteHandler handler);

    /// Match request method + path, parse query params, invoke handler.
    /// Returns 404 if no route matches.
    coro::CoroTask<HttpResponse> handle(const HttpRequest& req);

   private:
    struct Route {
        std::string method;
        std::string path;
        RouteHandler handler;
    };
    std::vector<Route> routes_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_ROUTER_H
