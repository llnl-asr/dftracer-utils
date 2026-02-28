// HTTP/1.1 request parser.
//
// Inspired by picohttpparser by Kazuho Oku, Tokuhiro Matsuno,
// Daisuke Murase, Shigeo Mitsunari (MIT license).
// https://github.com/h2o/picohttpparser

#ifndef DFTRACER_UTILS_SERVER_HTTP_PARSER_H
#define DFTRACER_UTILS_SERVER_HTTP_PARSER_H

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::server::parser {

struct Header {
    std::string_view name;
    std::string_view value;
};

struct ParsedRequest {
    std::string_view method;
    std::string_view path;
    int minor_version = 0;
    std::vector<Header> headers;
};

/// Parse an HTTP/1.1 request from a buffer.
///
/// Returns bytes consumed on success (> 0), -2 if the request is
/// incomplete (need more data), -1 on parse error.
int parse_request(const char* buf, std::size_t len, ParsedRequest& out);

}  // namespace dftracer::utils::server::parser

#endif  // DFTRACER_UTILS_SERVER_HTTP_PARSER_H
