#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace dftracer::utils::utilities::composites::dft::indexing {

void ChunkDimensionStats::observe(std::string_view value) {
    if (!value_counts) {
        value_counts.emplace();
    }

    // NOTE(perf): transparent lookup: find with string_view, only construct
    // string on insert
    auto it = value_counts->find(value);
    bool inserted = false;
    if (it == value_counts->end()) {
        auto [new_it, _] = value_counts->emplace(std::string(value), 0);
        it = new_it;
        inserted = true;
    }
    it->second++;

    if (inserted) {
        distinct_count = value_counts->size();
    }

    // NOTE(perf): min/max: fast-path for uint dimensions compare as integers
    if (value_type == "uint") {
        std::uint64_t val = 0;
        auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), val);
        if (ec == std::errc()) {
            if (min_value.empty()) {
                min_value = it->first;
                max_value = it->first;
            } else {
                std::uint64_t cur_min = 0, cur_max = 0;
                std::from_chars(min_value.data(),
                                min_value.data() + min_value.size(), cur_min);
                std::from_chars(max_value.data(),
                                max_value.data() + max_value.size(), cur_max);
                if (val < cur_min) min_value = it->first;
                if (val > cur_max) max_value = it->first;
            }
            return;
        }
    }

    const std::string& val_ref = it->first;
    if (min_value.empty() || val_ref < min_value) {
        min_value = val_ref;
    }
    if (max_value.empty() || val_ref > max_value) {
        max_value = val_ref;
    }
}

std::vector<std::uint8_t> ChunkDimensionStats::serialize_value_counts() const {
    if (!value_counts || value_counts->empty()) return {};

    std::vector<std::uint8_t> buf;
    auto num = static_cast<std::uint32_t>(value_counts->size());

    // Reserve rough estimate
    buf.reserve(4 + num * 20);

    // num_entries (u32 LE)
    buf.push_back(static_cast<std::uint8_t>(num & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((num >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((num >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((num >> 24) & 0xFF));

    for (const auto& [key, count] : *value_counts) {
        auto key_len = static_cast<std::uint16_t>(
            std::min<std::size_t>(key.size(), 0xFFFF));

        // key_len (u16 LE)
        buf.push_back(static_cast<std::uint8_t>(key_len & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((key_len >> 8) & 0xFF));

        // key bytes
        buf.insert(buf.end(), key.data(), key.data() + key_len);

        // count (u64 LE)
        auto c = static_cast<std::uint64_t>(count);
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<std::uint8_t>((c >> (i * 8)) & 0xFF));
        }
    }

    return buf;
}

std::optional<std::vector<std::uint8_t>>
ChunkDimensionStats::compress_value_counts(std::size_t cap_bytes) const {
    auto raw = serialize_value_counts();
    if (raw.empty()) return std::nullopt;

    // NOTE(perf): Reuse zlib stream across calls, deflateReset resets state
    // without reallocating internal buffers.
    struct ZlibDeflater {
        z_stream strm{};
        bool init = false;
        ~ZlibDeflater() {
            if (init) deflateEnd(&strm);
        }
    };
    static thread_local ZlibDeflater zd;
    if (!zd.init) {
        deflateInit(&zd.strm, Z_DEFAULT_COMPRESSION);
        zd.init = true;
    } else {
        deflateReset(&zd.strm);
    }
    auto& strm = zd.strm;

    uLongf compressed_len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressed_len);

    strm.next_in = raw.data();
    strm.avail_in = static_cast<uInt>(raw.size());
    strm.next_out = compressed.data();
    strm.avail_out = static_cast<uInt>(compressed_len);

    int rc = deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END) return std::nullopt;

    compressed.resize(strm.total_out);
    if (compressed.size() > cap_bytes) return std::nullopt;

    return compressed;
}

namespace {
std::uint16_t read_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      static_cast<std::uint16_t>(p[1] << 8));
}
std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t read_u64_le(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return v;
}
}  // namespace

std::unordered_map<std::string, std::uint64_t>
ChunkDimensionStats::deserialize_value_counts(const std::uint8_t* data,
                                              std::size_t len) {
    std::unordered_map<std::string, std::uint64_t> result;
    if (!data || len < 4) return result;

    std::size_t pos = 0;

    std::uint32_t num = read_u32_le(data + pos);
    pos += 4;

    for (std::uint32_t i = 0; i < num && pos + 2 <= len; ++i) {
        std::uint16_t key_len = read_u16_le(data + pos);
        pos += 2;

        if (pos + key_len + 8 > len) break;

        std::string key(reinterpret_cast<const char*>(data + pos), key_len);
        pos += key_len;

        std::uint64_t count = read_u64_le(data + pos);
        pos += 8;

        result[std::move(key)] = count;
    }

    return result;
}

std::unordered_map<std::string, std::uint64_t>
ChunkDimensionStats::decompress_value_counts(const std::uint8_t* data,
                                             std::size_t len) {
    if (!data || len == 0) return {};

    // Decompress: start with 4x estimate, grow if needed
    std::vector<std::uint8_t> decompressed(len * 4);
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
