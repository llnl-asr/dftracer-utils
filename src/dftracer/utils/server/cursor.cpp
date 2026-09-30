#include <dftracer/utils/core/common/base64.h>
#include <dftracer/utils/server/cursor.h>

#include <cstring>

namespace dftracer::utils::server {

// Wire format: 8 bytes file_index + 4 bytes chunk_index + 4 bytes line_offset
// (all little-endian), base64-encoded.
static constexpr std::size_t CURSOR_BYTES = 16;

std::string QueryCursor::encode() const {
    unsigned char raw[CURSOR_BYTES];
    std::memcpy(raw, &file_index, 8);
    std::memcpy(raw + 8, &chunk_index, 4);
    std::memcpy(raw + 12, &line_offset, 4);
    return base64_encode(raw, CURSOR_BYTES);
}

std::optional<QueryCursor> QueryCursor::decode(std::string_view cursor) {
    auto raw = base64_decode(cursor);
    if (!raw || raw->size() != CURSOR_BYTES) return std::nullopt;
    QueryCursor c;
    std::memcpy(&c.file_index, raw->data(), 8);
    std::memcpy(&c.chunk_index, raw->data() + 8, 4);
    std::memcpy(&c.line_offset, raw->data() + 12, 4);
    return c;
}

}  // namespace dftracer::utils::server
