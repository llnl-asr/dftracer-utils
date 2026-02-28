#ifndef DFTRACER_UTILS_SERVER_HTTP_RESPONSE_H
#define DFTRACER_UTILS_SERVER_HTTP_RESPONSE_H

#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::server {

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// Serialize HTTP response headers to string
    /// (CRLF-terminated). Automatically adds Content-Length
    /// if body is non-empty and not already set.
    std::string serialize_headers() const;

    /// Full response (headers + body).
    std::string serialize() const;

    static HttpResponse ok();
    static HttpResponse ok(
        const std::string &body,
        const std::string &content_type = "application/json");
    static HttpResponse not_found();
    static HttpResponse bad_request(const std::string &msg);
    static HttpResponse internal_error(const std::string &msg);
};

}  // namespace dftracer::utils::server

#endif
