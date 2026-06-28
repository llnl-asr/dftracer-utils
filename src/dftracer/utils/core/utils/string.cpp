#include <dftracer/utils/core/utils/string.h>

namespace dftracer::utils {
namespace {
inline bool is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool json_trim_impl(const char* data, std::size_t length, const char*& start,
                    std::size_t& trimmed_length, bool trim_trailing_comma) {
    start = data;
    const char* end = data + length - 1;

    while (start <= end && is_json_ws(*start)) {
        start++;
    }
    while (end >= start && is_json_ws(*end)) {
        end--;
    }

    // Trailing commas appear in the JSON array format.
    if (trim_trailing_comma && end >= start && *end == ',') {
        end--;
    }

    trimmed_length = (end >= start) ? (end - start + 1) : 0;

    if (trimmed_length == 0 ||
        (trimmed_length == 1 &&
         (*start == '[' || *start == '{' || *start == ']' || *start == '}')) ||
        (trimmed_length == 2 && start[0] == ']' && start[1] == '[')) {
        // Invalid/incomplete JSON or file boundary artifact
        return false;
    }

    return true;
}
}  // namespace

bool json_trim_and_validate(const char* data, std::size_t length,
                            const char*& start, std::size_t& trimmed_length) {
    return json_trim_impl(data, length, start, trimmed_length, false);
}

bool json_trim_and_validate_with_comma(const char* data, std::size_t length,
                                       const char*& start,
                                       std::size_t& trimmed_length) {
    return json_trim_impl(data, length, start, trimmed_length, true);
}
}  // namespace dftracer::utils
