#ifndef DFTRACER_UTILS_SERVER_HTTP_REQUEST_H
#define DFTRACER_UTILS_SERVER_HTTP_REQUEST_H

#include <dftracer/utils/server/cancel.h>

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::server {

struct HttpRequest {
    std::string_view method;
    std::string_view path;
    int minor_version = 0;
    std::vector<std::pair<std::string_view, std::string_view>> headers;

    /// Cooperative cancellation for this request's handler. Empty (never
    /// cancelled) unless set by the connection handler.
    CancelToken cancel_token;

    /// Parse from receive buffer.
    /// Returns bytes consumed on success, -1 on error,
    /// -2 if incomplete.
    int parse(const char *buf, std::size_t len);

    /// Find a header value by name (case-insensitive).
    std::string_view header(std::string_view name) const;

    /// Check if header matches value (case-insensitive).
    bool has_header(std::string_view name, std::string_view value) const;
};

}  // namespace dftracer::utils::server

#endif
