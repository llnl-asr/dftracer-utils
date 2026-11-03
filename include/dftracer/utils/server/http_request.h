// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_SERVER_HTTP_REQUEST_H
#define DFTRACER_UTILS_SERVER_HTTP_REQUEST_H

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/server/cancel.h>

#include <cstddef>
#include <utility>

namespace dftracer::utils::server {

struct HttpRequest {
    dstd::StringView method;
    dstd::StringView path;
    int minor_version = 0;
    dstd::Vector<dstd::Pair<dstd::StringView, dstd::StringView>> headers;

    /// Cooperative cancellation for this request's handler. Empty (never
    /// cancelled) unless set by the connection handler.
    CancelToken cancel_token;

    /// Parse from receive buffer.
    /// Returns bytes consumed on success, -1 on error,
    /// -2 if incomplete.
    int parse(const char *buf, std::size_t len);

    /// Find a header value by name (case-insensitive).
    dstd::StringView header(dstd::StringView name) const;

    /// Check if header matches value (case-insensitive).
    bool has_header(dstd::StringView name, dstd::StringView value) const;
};

}  // namespace dftracer::utils::server

#endif
