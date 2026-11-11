// SPDX-License-Identifier: MIT
// HTTP/1.1 request parser.
//
// Inspired by picohttpparser by Kazuho Oku, Tokuhiro Matsuno,
// Daisuke Murase, Shigeo Mitsunari (dual Perl/MIT, used under MIT).
// https://github.com/h2o/picohttpparser

#ifndef DFTRACER_UTILS_DOMAINS_SERVER_HTTP_PARSER_H
#define DFTRACER_UTILS_DOMAINS_SERVER_HTTP_PARSER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <utility>

namespace dftracer::utils::domains::server::parser {

struct Header {
    dstd::StringView name;
    dstd::StringView value;
};

struct ParsedRequest {
    dstd::StringView method;
    dstd::StringView path;
    int minor_version = 0;
    dstd::Vector<Header> headers;
};

/// Parse an HTTP/1.1 request from a buffer.
///
/// Returns bytes consumed on success (> 0), -2 if the request is
/// incomplete (need more data), -1 on parse error.
int parse_request(const char* buf, std::size_t len, ParsedRequest& out);

}  // namespace dftracer::utils::domains::server::parser

#endif  // DFTRACER_UTILS_DOMAINS_SERVER_HTTP_PARSER_H
