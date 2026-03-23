#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {
bool is_numeric(const std::string& vtype) {
    return vtype == "uint" || vtype == "int" || vtype == "double";
}

bool less_than(const std::string& a, const std::string& b,
               const std::string& vtype) {
    if (is_numeric(vtype)) {
        try {
            return std::stod(a) < std::stod(b);
        } catch (...) {
        }
    }
    return a < b;
}
}  // namespace

void ChunkDimensionStats::observe(std::string_view value) {
    std::string val_str(value);

    if (!value_counts) {
        value_counts.emplace();
    }
    (*value_counts)[val_str]++;

    distinct_count = value_counts->size();

    if (min_value.empty() || less_than(val_str, min_value, value_type)) {
        min_value = val_str;
    }
    if (max_value.empty() || less_than(max_value, val_str, value_type)) {
        max_value = val_str;
    }
}

std::vector<uint8_t> ChunkDimensionStats::serialize_value_counts() const {
    if (!value_counts || value_counts->empty()) return {};

    std::vector<uint8_t> buf;
    auto num = static_cast<uint32_t>(value_counts->size());

    // Reserve rough estimate
    buf.reserve(4 + num * 20);

    // num_entries (u32 LE)
    buf.push_back(static_cast<uint8_t>(num & 0xFF));
    buf.push_back(static_cast<uint8_t>((num >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((num >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((num >> 24) & 0xFF));

    for (const auto& [key, count] : *value_counts) {
        auto key_len =
            static_cast<uint16_t>(std::min<std::size_t>(key.size(), 0xFFFF));

        // key_len (u16 LE)
        buf.push_back(static_cast<uint8_t>(key_len & 0xFF));
        buf.push_back(static_cast<uint8_t>((key_len >> 8) & 0xFF));

        // key bytes
        buf.insert(buf.end(), key.data(), key.data() + key_len);

        // count (u64 LE)
        auto c = static_cast<uint64_t>(count);
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<uint8_t>((c >> (i * 8)) & 0xFF));
        }
    }

    return buf;
}

std::optional<std::vector<uint8_t>> ChunkDimensionStats::compress_value_counts(
    std::size_t cap_bytes) const {
    auto raw = serialize_value_counts();
    if (raw.empty()) return std::nullopt;

    uLongf compressed_len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressed_len);

    int rc = compress2(compressed.data(), &compressed_len, raw.data(),
                       static_cast<uLong>(raw.size()), Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return std::nullopt;

    compressed.resize(compressed_len);
    if (compressed.size() > cap_bytes) return std::nullopt;

    return compressed;
}

namespace {
uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}
}  // namespace

std::unordered_map<std::string, std::uint64_t>
ChunkDimensionStats::deserialize_value_counts(const uint8_t* data,
                                              std::size_t len) {
    std::unordered_map<std::string, std::uint64_t> result;
    if (!data || len < 4) return result;

    std::size_t pos = 0;

    uint32_t num = read_u32_le(data + pos);
    pos += 4;

    for (uint32_t i = 0; i < num && pos + 2 <= len; ++i) {
        uint16_t key_len = read_u16_le(data + pos);
        pos += 2;

        if (pos + key_len + 8 > len) break;

        std::string key(reinterpret_cast<const char*>(data + pos), key_len);
        pos += key_len;

        uint64_t count = read_u64_le(data + pos);
        pos += 8;

        result[std::move(key)] = count;
    }

    return result;
}

std::unordered_map<std::string, std::uint64_t>
ChunkDimensionStats::decompress_value_counts(const uint8_t* data,
                                             std::size_t len) {
    if (!data || len == 0) return {};

    // Decompress: start with 4x estimate, grow if needed
    std::vector<uint8_t> decompressed(len * 4);
    uLongf decompressed_len = static_cast<uLongf>(decompressed.size());

    int rc = uncompress(decompressed.data(), &decompressed_len, data,
                        static_cast<uLong>(len));

    // Retry with larger buffer if needed
    if (rc == Z_BUF_ERROR) {
        decompressed.resize(len * 16);
        decompressed_len = static_cast<uLongf>(decompressed.size());
        rc = uncompress(decompressed.data(), &decompressed_len, data,
                        static_cast<uLong>(len));
    }

    if (rc != Z_OK) return {};

    return deserialize_value_counts(decompressed.data(), decompressed_len);
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
