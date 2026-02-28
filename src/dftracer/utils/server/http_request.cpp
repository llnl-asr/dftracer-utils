#include <dftracer/utils/server/http_parser.h>
#include <dftracer/utils/server/http_request.h>

#include <algorithm>
#include <cctype>

namespace dftracer::utils::server {

namespace {

bool icase_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) ==
               std::tolower(static_cast<unsigned char>(cb));
    });
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

std::string_view HttpRequest::header(std::string_view name) const {
    for (const auto &[k, v] : headers) {
        if (icase_equal(k, name)) return v;
    }
    return {};
}

bool HttpRequest::has_header(std::string_view name,
                             std::string_view value) const {
    for (const auto &[k, v] : headers) {
        if (icase_equal(k, name) && icase_equal(v, value)) return true;
    }
    return false;
}

}  // namespace dftracer::utils::server
