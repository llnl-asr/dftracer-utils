#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_ESCAPE_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_ESCAPE_H

#include <string>
#include <string_view>

namespace dftracer::utils::utilities::common::json {

// Escape a string for embedding in a JSON string literal (quotes, backslash,
// and the standard control-character shorthands).
inline std::string escape_json_string(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
                break;
        }
    }
    return result;
}

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_ESCAPE_H
