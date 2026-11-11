// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_FMT_H
#define DFTRACER_UTILS_JSON_FMT_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/json/json_escape.h>

namespace dftracer::utils::json {

/// Escape policy for dstd::fmt_with: string arguments are JSON-escaped (no
/// quotes added); worst case is 6 bytes a byte.
struct JsonEscape {
    static std::size_t bound(dstd::StringView s) { return 6 * s.size(); }
    static char* write(char* d, dstd::StringView s) {
        return escape_json_to(d, s);
    }
};

/// dstd::fmt with string arguments JSON-escaped; numbers, bools and chars
/// print as in dstd::fmt and dstd::fmt_detail::RawText is not escaped.
template <dstd::FixedString S, class... A>
dstd::String fmt(const A&... a) {
    return dstd::fmt_with<JsonEscape, S>(a...);
}

/// dstd::fmt_to with string arguments JSON-escaped. The cut, when the text
/// does not fit, falls on the escaped text.
template <dstd::FixedString S, class... A>
dstd::FmtResult fmt_to(char* buf, std::size_t cap, const A&... a) {
    return dstd::fmt_to_with<JsonEscape, S>(buf, cap, a...);
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_FMT_H
