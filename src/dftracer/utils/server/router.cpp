#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>

namespace dftracer::utils::server {

// ============================================================================
// QueryParams
// ============================================================================

static std::string url_decode(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '%' && i + 2 < sv.size()) {
            char hex[3] = {sv[i + 1], sv[i + 2], '\0'};
            char* end = nullptr;
            unsigned long val = std::strtoul(hex, &end, 16);
            if (end == hex + 2) {
                result.push_back(static_cast<char>(val));
                i += 2;
                continue;
            }
        } else if (sv[i] == '+') {
            result.push_back(' ');
            continue;
        }
        result.push_back(sv[i]);
    }
    return result;
}

QueryParams QueryParams::parse(std::string_view query) {
    QueryParams params;
    while (!query.empty()) {
        auto amp = query.find('&');
        auto pair = query.substr(0, amp);
        query = (amp == std::string_view::npos) ? std::string_view{}
                                                : query.substr(amp + 1);

        auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            params.params_.emplace_back(url_decode(pair), "");
        } else {
            params.params_.emplace_back(url_decode(pair.substr(0, eq)),
                                        url_decode(pair.substr(eq + 1)));
        }
    }
    return params;
}

std::string_view QueryParams::get(std::string_view key,
                                  std::string_view default_value) const {
    for (const auto& [k, v] : params_) {
        if (k == key) return v;
    }
    return default_value;
}

bool QueryParams::has(std::string_view key) const {
    for (const auto& [k, v] : params_) {
        if (k == key) return true;
    }
    return false;
}

int QueryParams::get_int(std::string_view key, int default_value) const {
    auto sv = get(key);
    if (sv.empty()) return default_value;
    int val = default_value;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

double QueryParams::get_double(std::string_view key,
                               double default_value) const {
    auto sv = get(key);
    if (sv.empty()) return default_value;
    // std::from_chars for doubles not universally available in C++17
    // compilers. Use strtod as fallback.
    char* end = nullptr;
    std::string tmp(sv);
    double val = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return default_value;
    return val;
}

// ============================================================================
// Router
// ============================================================================

void Router::get(const std::string& path, RouteHandler handler) {
    routes_.push_back(Route{"GET", path, std::move(handler), {}});
}

void Router::get(const std::string& path, RouteHandler handler, RouteDoc doc) {
    routes_.push_back(Route{"GET", path, std::move(handler), std::move(doc)});
}

void Router::post(const std::string& path, RouteHandler handler) {
    routes_.push_back(Route{"POST", path, std::move(handler), {}});
}

void Router::post(const std::string& path, RouteHandler handler, RouteDoc doc) {
    routes_.push_back(Route{"POST", path, std::move(handler), std::move(doc)});
}

namespace {

// RFC 6750: the auth scheme name is case-insensitive.
bool strip_bearer_prefix(std::string_view h, std::string_view& token) {
    constexpr std::string_view BEARER = "Bearer ";
    if (h.size() <= BEARER.size()) return false;
    for (std::size_t i = 0; i < BEARER.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(h[i])) !=
            std::tolower(static_cast<unsigned char>(BEARER[i])))
            return false;
    }
    token = h.substr(BEARER.size());
    return true;
}

}  // namespace

coro::CoroTask<HttpResponse> Router::handle(const HttpRequest& req) {
    // Split path and query string.
    auto path = req.path;
    std::string_view query_str;
    auto qpos = path.find('?');
    if (qpos != std::string_view::npos) {
        query_str = path.substr(qpos + 1);
        path = path.substr(0, qpos);
    }

    auto params = QueryParams::parse(query_str);

    // Before the token check: browsers omit Authorization from the preflight.
    if (req.method == "OPTIONS") {
        co_return HttpResponse{
            .status_code = 204,
            .status_text = "No Content",
            .headers = {{"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
                        {"Access-Control-Allow-Headers",
                         "Authorization, "
                         "Content-Type"},
                        {"Access-Control-Max-Age", "86400"}},
            .body = ""};
    }

    // Optional access token: accept ?token= or "Authorization: Bearer <token>".
    if (!auth_token_.empty()) {
        bool ok = params.get("token") == auth_token_;
        if (!ok) {
            std::string_view token;
            if (strip_bearer_prefix(req.header("Authorization"), token))
                ok = token == auth_token_;
        }
        if (!ok) {
            co_return HttpResponse{.status_code = 401,
                                   .status_text = "Unauthorized",
                                   .headers = {{"Content-Type", "text/plain"}},
                                   .body = "Unauthorized"};
        }
    }

    // Match routes (exact prefix match).
    for (const auto& route : routes_) {
        if (req.method == route.method && path == route.path) {
            co_return co_await route.handler(req, params);
        }
    }

    // Try prefix matches for parameterized routes
    // (e.g., "/api/v1/files/:file/info" matches "/api/v1/files/foo/info")
    // For now, use exact match only — parameterized routes can be added later.

    co_return HttpResponse::not_found();
}

}  // namespace dftracer::utils::server
