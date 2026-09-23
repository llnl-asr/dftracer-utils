#ifndef DFTRACER_UTILS_JSON_JSON_ESCAPE_H
#define DFTRACER_UTILS_JSON_JSON_ESCAPE_H

#include <cstdio>
#include <string>
#include <string_view>

namespace dftracer::utils::json {

/// Append the JSON-escaped form of `s` (quotes, backslash, the standard
/// control-character shorthands, and \uXXXX for every other control
/// character below 0x20, all required by the JSON spec) to `out`. No
/// temporary allocation, so callers building a JSON line in a loop don't
/// churn a string per field.
inline void append_json_escaped(std::string& out, std::string_view s) {
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
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
}

/// Escape a string for embedding in a JSON string literal.
inline std::string escape_json_string(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    append_json_escaped(result, s);
    return result;
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_ESCAPE_H
