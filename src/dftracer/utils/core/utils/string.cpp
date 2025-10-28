#include <dftracer/utils/core/utils/string.h>

namespace dftracer::utils {
bool json_trim_and_validate(const char* data, std::size_t length,
                            const char*& start, std::size_t& trimmed_length) {
    start = data;
    const char* end = data + length - 1;

    while (start <= end && (*start == ' ' || *start == '\t' || *start == '\n' ||
                            *start == '\r')) {
        start++;
    }
    while (end >= start &&
           (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
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

bool json_trim_and_validate_with_comma(const char* data, std::size_t length,
                                       const char*& start,
                                       std::size_t& trimmed_length) {
    start = data;
    const char* end = data + length - 1;

    while (start <= end && (*start == ' ' || *start == '\t' || *start == '\n' ||
                            *start == '\r')) {
        start++;
    }
    while (end >= start &&
           (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    // Also trim trailing commas (for JSON array format)
    if (end >= start && *end == ',') {
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
}  // namespace dftracer::utils
