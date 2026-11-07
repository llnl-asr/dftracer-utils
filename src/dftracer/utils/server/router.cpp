// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>

#include <algorithm>
#include <cstdlib>

namespace dftracer::utils::server {

// ============================================================================
// QueryParams
// ============================================================================

static dstd::String url_decode(dstd::StringView sv) {
    dstd::String result;
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

QueryParams QueryParams::parse(dstd::StringView query) {
    QueryParams params;
    while (!query.empty()) {
        auto amp = query.find('&');
        auto pair = query.substr(0, amp);
        query = (amp == dstd::StringView::npos) ? dstd::StringView{}
                                                : query.substr(amp + 1);

        auto eq = pair.find('=');
        if (eq == dstd::StringView::npos) {
            params.params_.emplace_back(url_decode(pair), "");
        } else {
            params.params_.emplace_back(url_decode(pair.substr(0, eq)),
                                        url_decode(pair.substr(eq + 1)));
        }
    }
    return params;
}

dstd::StringView QueryParams::get(dstd::StringView key,
                                  dstd::StringView default_value) const {
    for (const auto& [k, v] : params_) {
        if (k == key) return v;
    }
    return default_value;
}

bool QueryParams::has(dstd::StringView key) const {
    for (const auto& [k, v] : params_) {
        if (k == key) return true;
    }
    return false;
}

dstd::String QueryParams::canonical_key() const {
    dstd::Vector<const dstd::Pair<dstd::String, dstd::String>*> sorted;
    sorted.reserve(params_.size());
    for (const auto& p : params_) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        if (a->first != b->first) return a->first < b->first;
        return a->second < b->second;
    });
    dstd::String key;
    for (const auto* p : sorted) {
        key.append(p->first);
        key.push_back('=');
        key.append(p->second);
        key.push_back('\x1f');
    }
    return key;
}

int QueryParams::get_int(dstd::StringView key, int default_value) const {
    auto sv = get(key);
    if (sv.empty()) return default_value;
    int val = default_value;
    dstd::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

double QueryParams::get_double(dstd::StringView key,
                               double default_value) const {
    auto sv = get(key);
    if (sv.empty()) return default_value;
    // std::from_chars for doubles not universally available in C++17
    // compilers. Use strtod as fallback.
    char* end = nullptr;
    dstd::String tmp(sv);
    double val = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return default_value;
    return val;
}

// ============================================================================
// Router
// ============================================================================

void Router::get(const dstd::String& path, RouteHandler handler) {
    routes_.push_back(Route{"GET", path, std::move(handler), {}});
}

void Router::get(const dstd::String& path, RouteHandler handler, RouteDoc doc) {
    routes_.push_back(Route{"GET", path, std::move(handler), std::move(doc)});
}

void Router::post(const dstd::String& path, RouteHandler handler) {
    routes_.push_back(Route{"POST", path, std::move(handler), {}});
}

void Router::post(const dstd::String& path, RouteHandler handler,
                  RouteDoc doc) {
    routes_.push_back(Route{"POST", path, std::move(handler), std::move(doc)});
}

namespace {

// RFC 9110 section 11.1: the auth scheme name is case-insensitive.
bool strip_bearer_prefix(dstd::StringView h, dstd::StringView& token) {
    constexpr dstd::StringView BEARER = "Bearer ";
    if (h.size() <= BEARER.size()) return false;
    if (!::dftracer::utils::common::detail::iequals(h.substr(0, BEARER.size()),
                                                    BEARER))
        return false;
    token = h.substr(BEARER.size());
    return true;
}

}  // namespace

coro::CoroTask<HttpResponse> Router::handle(const HttpRequest& req) {
    // Split path and query string.
    auto path = req.path;
    dstd::StringView query_str;
    auto qpos = path.find('?');
    if (qpos != dstd::StringView::npos) {
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
                         "Content-Type, "
                         "X-Request-Id"},
                        {"Access-Control-Max-Age", "86400"}},
            .body = ""};
    }

    // Optional access token: accept ?token= or "Authorization: Bearer <token>".
    if (!auth_token_.empty()) {
        bool ok = params.get("token") == auth_token_;
        if (!ok) {
            dstd::StringView token;
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

    // Routes match exactly. Parameterized routes such as
    // "/api/files/:file/info" are not supported.

    co_return HttpResponse::not_found();
}

}  // namespace dftracer::utils::server
