// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/domains/server/http_parser.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/string.h>

#include <algorithm>

namespace dftracer::utils::domains::server {

namespace {

bool icase_equal(dstd::StringView a, dstd::StringView b) {
    return ::dftracer::utils::dstd::ascii_iequals(a, b);
}

}  // namespace

int HttpRequest::parse(const char *buf, std::size_t len) {
    parser::ParsedRequest parsed;
    int ret = parser::parse_request(buf, len, parsed);
    if (ret < 0) return ret;

    method = parsed.method;
    path = parsed.path;
    minor_version = parsed.minor_version;

    headers.clear();
    headers.reserve(parsed.headers.size());
    for (const auto &h : parsed.headers) {
        headers.emplace_back(h.name, h.value);
    }

    return ret;
}

dstd::StringView HttpRequest::header(dstd::StringView name) const {
    for (const auto &[k, v] : headers) {
        if (icase_equal(k, name)) return v;
    }
    return {};
}

bool HttpRequest::has_header(dstd::StringView name,
                             dstd::StringView value) const {
    for (const auto &[k, v] : headers) {
        if (icase_equal(k, name) && icase_equal(v, value)) return true;
    }
    return false;
}

}  // namespace dftracer::utils::domains::server
