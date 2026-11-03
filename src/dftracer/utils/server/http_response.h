// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_SERVER_HTTP_RESPONSE_H
#define DFTRACER_UTILS_SERVER_HTTP_RESPONSE_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <utility>

namespace dftracer::utils::server {

struct HttpResponse {
    int status_code = 200;
    dstd::String status_text = "OK";
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> headers;
    dstd::String body;

    struct StreamChunk {
        dstd::Span<const dstd::StringView> views;
    };

    using StreamGenerator = coro::AsyncGenerator<StreamChunk>;
    dstd::UniquePtr<StreamGenerator> stream;

    bool is_streaming() const { return stream != nullptr; }

    /// Serialize HTTP response headers to string
    /// (CRLF-terminated). Automatically adds Content-Length
    /// if body is non-empty and not already set.
    dstd::String serialize_headers() const;

    /// Full response (headers + body).
    dstd::String serialize() const;

    static HttpResponse ok();
    static HttpResponse ok(dstd::String body, const dstd::String &content_type =
                                                  "application/json");
    static HttpResponse not_found();
    static HttpResponse bad_request(const dstd::String &msg);
    static HttpResponse internal_error(const dstd::String &msg);

    static HttpResponse streaming(
        dstd::UniquePtr<StreamGenerator> gen,
        const dstd::String &content_type = "application/x-ndjson");
};

}  // namespace dftracer::utils::server

#endif
