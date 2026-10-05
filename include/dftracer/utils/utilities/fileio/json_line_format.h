#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_JSON_LINE_FORMAT_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_JSON_LINE_FORMAT_H

#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/utilities/fileio/line_format.h>

namespace dftracer::utils::utilities::fileio::line_format {

/// Strings are JSON-escaped (no quotes added); worst case is 6 bytes a byte.
struct JsonEscape {
    static std::size_t bound(std::string_view s) { return 6 * s.size(); }
    static char* write(char* d, std::string_view s) {
        return json::escape_json_to(d, s);
    }
};

using JsonLineFormat = BasicLineFormat<JsonEscape>;

}  // namespace dftracer::utils::utilities::fileio::line_format

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_JSON_LINE_FORMAT_H
