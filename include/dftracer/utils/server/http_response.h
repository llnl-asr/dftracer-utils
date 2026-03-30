#ifndef DFTRACER_UTILS_SERVER_HTTP_RESPONSE_H
#define DFTRACER_UTILS_SERVER_HTTP_RESPONSE_H

#include <dftracer/utils/core/coro/async_generator.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::server {

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    struct StreamChunk {
        std::span<const std::string_view> views;
    };

    using StreamGenerator = coro::AsyncGenerator<StreamChunk>;
    std::unique_ptr<StreamGenerator> stream;

    bool is_streaming() const { return stream != nullptr; }

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

    static HttpResponse streaming(
        std::unique_ptr<StreamGenerator> gen,
        const std::string &content_type = "application/x-ndjson");
};

}  // namespace dftracer::utils::server

#endif
