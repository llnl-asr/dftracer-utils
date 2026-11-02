// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_SERVER_CURSOR_H
#define DFTRACER_UTILS_SERVER_CURSOR_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::server {

/// Cursor for paginated trace queries. Encodes the current position
/// in the scan across files and chunks for efficient resumption.
struct QueryCursor {
    std::size_t file_index = 0;
    std::uint32_t chunk_index = 0;
    std::uint32_t line_offset = 0;

    /// Base64-encode the cursor for URL-safe transmission.
    std::string encode() const;

    /// Decode a cursor from a Base64 string.
    static dstd::Optional<QueryCursor> decode(dstd::StringView cursor);
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_CURSOR_H
