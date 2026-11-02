// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_JSON_ESCAPE_H
#define DFTRACER_UTILS_JSON_JSON_ESCAPE_H

#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::json {

/// Append the JSON-escaped form of `s` (quotes, backslash, the standard
/// control-character shorthands, and \uXXXX for every other control
/// character below 0x20, all required by the JSON spec) to `out`. No
/// temporary allocation, so callers building a JSON line in a loop don't
/// churn a string per field.
inline void append_json_escaped(dstd::String& out, dstd::StringView s) {
    static constexpr char HEX[] = "0123456789abcdef";
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default: {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20) {
                    const char escaped[6] = {'\\', 'u',         '0',
                                             '0',  HEX[u >> 4], HEX[u & 15]};
                    out.append(escaped, sizeof(escaped));
                } else {
                    out += c;
                }
                break;
            }
        }
    }
}

/// Like append_json_escaped, into `dst` with room for 6 * s.size() bytes.
/// Returns the end of the written bytes.
inline char* escape_json_to(char* dst, dstd::StringView s) {
    static constexpr char HEX[] = "0123456789abcdef";
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"':
                *dst++ = '\\';
                *dst++ = '"';
                break;
            case '\\':
                *dst++ = '\\';
                *dst++ = '\\';
                break;
            case '\b':
                *dst++ = '\\';
                *dst++ = 'b';
                break;
            case '\f':
                *dst++ = '\\';
                *dst++ = 'f';
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;
            default:
                if (u < 0x20) {
                    *dst++ = '\\';
                    *dst++ = 'u';
                    *dst++ = '0';
                    *dst++ = '0';
                    *dst++ = HEX[u >> 4];
                    *dst++ = HEX[u & 15];
                } else {
                    *dst++ = c;
                }
                break;
        }
    }
    return dst;
}

/// Escape a string for embedding in a JSON string literal.
inline dstd::String escape_json_string(dstd::StringView s) {
    dstd::String result;
    result.reserve(s.size());
    append_json_escaped(result, s);
    return result;
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_ESCAPE_H
