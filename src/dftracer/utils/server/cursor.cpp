#include <dftracer/utils/server/cursor.h>

#include <array>
#include <cstring>

namespace dftracer::utils::server {

namespace {

// Minimal base64 encode/decode for cursor serialization.
static constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const void* data, std::size_t len) {
    auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    for (std::size_t i = 0; i < len; i += 3) {
        unsigned val = static_cast<unsigned>(bytes[i]) << 16;
        if (i + 1 < len) val |= static_cast<unsigned>(bytes[i + 1]) << 8;
        if (i + 2 < len) val |= static_cast<unsigned>(bytes[i + 2]);
        out.push_back(kBase64Chars[(val >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(val >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kBase64Chars[(val >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kBase64Chars[val & 0x3F] : '=');
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view sv) {
    static constexpr unsigned char kDecTable[256] = {
        // clang-format off
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        // clang-format on
    };

    if (sv.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(sv.size() / 4 * 3);
    for (std::size_t i = 0; i < sv.size(); i += 4) {
        auto a = kDecTable[static_cast<unsigned char>(sv[i])];
        auto b = kDecTable[static_cast<unsigned char>(sv[i + 1])];
        auto c = kDecTable[static_cast<unsigned char>(sv[i + 2])];
        auto d = kDecTable[static_cast<unsigned char>(sv[i + 3])];
        if (a == 64 || b == 64) return std::nullopt;
        unsigned val = (a << 18) | (b << 12);
        out.push_back(static_cast<char>(val >> 16));
        if (c != 64) {
            val |= (c << 6);
            out.push_back(static_cast<char>((val >> 8) & 0xFF));
        }
        if (d != 64) {
            val |= d;
            out.push_back(static_cast<char>(val & 0xFF));
        }
    }
    return out;
}

}  // namespace

// Wire format: 8 bytes file_index + 4 bytes chunk_index + 4 bytes line_offset
// (all little-endian), base64-encoded.
static constexpr std::size_t kCursorBytes = 16;

std::string QueryCursor::encode() const {
    unsigned char raw[kCursorBytes];
    std::memcpy(raw, &file_index, 8);
    std::memcpy(raw + 8, &chunk_index, 4);
    std::memcpy(raw + 12, &line_offset, 4);
    return base64_encode(raw, kCursorBytes);
}

std::optional<QueryCursor> QueryCursor::decode(std::string_view cursor) {
    auto raw = base64_decode(cursor);
    if (!raw || raw->size() != kCursorBytes) return std::nullopt;
    QueryCursor c;
    std::memcpy(&c.file_index, raw->data(), 8);
    std::memcpy(&c.chunk_index, raw->data() + 8, 4);
    std::memcpy(&c.line_offset, raw->data() + 12, 4);
    return c;
}

}  // namespace dftracer::utils::server
