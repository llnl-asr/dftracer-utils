// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/line.h>

namespace dftracer::utils::json {
namespace {
inline bool is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool json_trim_impl(dstd::StringView line, dstd::StringView& trimmed,
                    bool trim_trailing_comma) {
    const char* start = line.data();
    const char* end = start + line.size() - 1;

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

    const std::size_t trimmed_size =
        (end >= start) ? static_cast<std::size_t>(end - start + 1) : 0;
    trimmed = dstd::StringView(start, trimmed_size);

    if (trimmed_size == 0 ||
        (trimmed_size == 1 &&
         (*start == '[' || *start == '{' || *start == ']' || *start == '}')) ||
        (trimmed_size == 2 && start[0] == ']' && start[1] == '[')) {
        // Invalid/incomplete JSON or file boundary artifact
        return false;
    }

    return true;
}
}  // namespace

bool trim_and_validate(dstd::StringView line, dstd::StringView& trimmed) {
    return json_trim_impl(line, trimmed, false);
}

bool trim_and_validate_with_comma(dstd::StringView line,
                                  dstd::StringView& trimmed) {
    return json_trim_impl(line, trimmed, true);
}
}  // namespace dftracer::utils::json
