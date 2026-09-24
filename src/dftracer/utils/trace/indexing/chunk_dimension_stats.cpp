#include <dftracer/utils/core/common/little_endian.h>
#include <dftracer/utils/trace/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace dftracer::utils::trace::indexing {

bool dimension_value_less(std::string_view a, std::string_view b,
                          std::string_view value_type) {
    if (value_type == "uint") {
        std::uint64_t x = 0, y = 0;
        std::from_chars(a.data(), a.data() + a.size(), x);
        std::from_chars(b.data(), b.data() + b.size(), y);
        return x < y;
    }
    if (value_type == "int") {
        std::int64_t x = 0, y = 0;
        std::from_chars(a.data(), a.data() + a.size(), x);
        std::from_chars(b.data(), b.data() + b.size(), y);
        return x < y;
    }
    if (value_type == "double")
        return std::stod(std::string(a)) < std::stod(std::string(b));
    return a < b;
}

void ChunkDimensionStats::observe(std::string_view value) {
    if (last_key_ != nullptr && *last_key_ == value) {
        ++*last_counter_;
        return;
    }

    if (!value_counts) {
        value_counts.emplace();
    }

    auto it = value_counts->find(value);
    if (it != value_counts->end()) {
        it->second++;
        last_key_ = &it->first;
        last_counter_ = &it->second;
        return;
    }

    auto [new_it, _] = value_counts->emplace(std::string(value), 1);
    it = new_it;
    distinct_count = value_counts->size();
    last_key_ = &it->first;
    last_counter_ = &it->second;

    const std::string& val_ref = it->first;

    if (value_type == "uint") {
        std::uint64_t val = 0;
        auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), val);
        if (ec == std::errc()) {
            if (min_value.empty()) {
                min_value = val_ref;
                max_value = val_ref;
            } else {
                std::uint64_t cur_min = 0, cur_max = 0;
                std::from_chars(min_value.data(),
                                min_value.data() + min_value.size(), cur_min);
                std::from_chars(max_value.data(),
                                max_value.data() + max_value.size(), cur_max);
                if (val < cur_min) min_value = val_ref;
                if (val > cur_max) max_value = val_ref;
            }
            return;
        }
    }

    if (min_value.empty() || val_ref < min_value) {
        min_value = val_ref;
    }
    if (max_value.empty() || val_ref > max_value) {
        max_value = val_ref;
    }
}

void ChunkDimensionStats::observe_range_only(std::uint64_t value) {
    distinct_count++;
    auto str = std::to_string(value);
    if (min_value.empty()) {
        min_value = str;
        max_value = str;
    } else {
        std::uint64_t cur_min = 0, cur_max = 0;
        std::from_chars(min_value.data(), min_value.data() + min_value.size(),
                        cur_min);
        std::from_chars(max_value.data(), max_value.data() + max_value.size(),
                        cur_max);
        if (value < cur_min) min_value = str;
        if (value > cur_max) max_value = str;
    }
}

std::vector<std::uint8_t> ChunkDimensionStats::serialize_value_counts() const {
    if (!value_counts || value_counts->empty()) return {};

    std::vector<std::uint8_t> buf;
    auto num = static_cast<std::uint32_t>(value_counts->size());

    buf.reserve(4 + num * 20);

    buf.push_back(static_cast<std::uint8_t>(num & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((num >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((num >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((num >> 24) & 0xFF));

    for (const auto& [key, count] : *value_counts) {
        auto key_len = static_cast<std::uint16_t>(
            std::min<std::size_t>(key.size(), 0xFFFF));

        buf.push_back(static_cast<std::uint8_t>(key_len & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((key_len >> 8) & 0xFF));

        buf.insert(buf.end(), key.data(), key.data() + key_len);

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

    namespace compress = dftracer::utils::utilities::fileio::compress;
    compress::GzipMemberCompressor compressor;
    if (!compressor.valid()) return std::nullopt;

    auto member = compressor.compress_member(raw.data(), raw.size());
    if (!member) return std::nullopt;

    // Prefix the decoded length so the reader can size the output buffer
    // exactly instead of guessing.
    std::vector<std::uint8_t> out;
    out.reserve(4 + member->size());
    const auto n = static_cast<std::uint32_t>(raw.size());
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((n >> (i * 8)) & 0xFF));
    }
    out.insert(out.end(), member->begin(), member->end());

    if (out.size() > cap_bytes) return std::nullopt;
    return out;
}

namespace {
std::uint16_t read_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      static_cast<std::uint16_t>(p[1] << 8));
}
std::uint64_t read_u64_le(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return v;
}
}  // namespace

dftracer::utils::StringViewMap<std::uint64_t>
ChunkDimensionStats::deserialize_value_counts(const std::uint8_t* data,
                                              std::size_t len) {
    dftracer::utils::StringViewMap<std::uint64_t> result;
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

dftracer::utils::StringViewMap<std::uint64_t>
ChunkDimensionStats::decompress_value_counts(const std::uint8_t* data,
                                             std::size_t len) {
    if (!data || len < 4) return {};

    std::uint32_t decoded_len = read_u32_le(data);

    namespace compress = dftracer::utils::utilities::fileio::compress;
    compress::GzipMemberDecompressor decompressor;
    if (!decompressor.valid()) return {};

    std::vector<std::uint8_t> decompressed(decoded_len);
    auto res = decompressor.decompress(data + 4, len - 4, decompressed.data(),
                                       decompressed.size());
    if (!res || res->out_bytes != decoded_len) return {};

    return deserialize_value_counts(decompressed.data(), res->out_bytes);
}

}  // namespace dftracer::utils::trace::indexing
