#ifndef DFTRACER_UTILS_UTILITIES_COMMON_SERIALIZATION_BINARY_CODEC_H
#define DFTRACER_UTILS_UTILITIES_COMMON_SERIALIZATION_BINARY_CODEC_H

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::common::serialization {

// =============================================================================
// Binary Writer Utilities
// =============================================================================

inline void put_u8(std::string& out, std::uint8_t v) {
    out.push_back(static_cast<char>(v));
}

inline void put_be16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

inline void put_be32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

inline void put_be64(std::string& out, std::uint64_t v) {
    out.push_back(static_cast<char>(v >> 56));
    out.push_back(static_cast<char>(v >> 48));
    out.push_back(static_cast<char>(v >> 40));
    out.push_back(static_cast<char>(v >> 32));
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

inline void put_double(std::string& out, double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    put_be64(out, bits);
}

inline void put_str(std::string& out, std::string_view s) {
    put_be16(out, static_cast<std::uint16_t>(s.size()));
    out.append(s.data(), s.size());
}

inline void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

inline void put_blob(std::string& out, std::span<const std::uint8_t> data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    out.append(reinterpret_cast<const char*>(data.data()), data.size());
}

// =============================================================================
// Raw Pointer Writer Utilities (for pre-sized buffers)
// =============================================================================

inline char* write_varint(char* p, std::uint64_t v) {
    while (v >= 0x80) {
        *p++ = static_cast<char>(v | 0x80);
        v >>= 7;
    }
    *p++ = static_cast<char>(v);
    return p;
}

inline char* write_be16(char* p, std::uint16_t v) {
    p[0] = static_cast<char>(v >> 8);
    p[1] = static_cast<char>(v);
    return p + 2;
}

inline char* write_be32(char* p, std::uint32_t v) {
    p[0] = static_cast<char>(v >> 24);
    p[1] = static_cast<char>(v >> 16);
    p[2] = static_cast<char>(v >> 8);
    p[3] = static_cast<char>(v);
    return p + 4;
}

inline char* write_be64(char* p, std::uint64_t v) {
    p[0] = static_cast<char>(v >> 56);
    p[1] = static_cast<char>(v >> 48);
    p[2] = static_cast<char>(v >> 40);
    p[3] = static_cast<char>(v >> 32);
    p[4] = static_cast<char>(v >> 24);
    p[5] = static_cast<char>(v >> 16);
    p[6] = static_cast<char>(v >> 8);
    p[7] = static_cast<char>(v);
    return p + 8;
}

inline char* write_double(char* p, double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    return write_be64(p, bits);
}

inline char* write_str(char* p, std::string_view s) {
    const auto n = static_cast<std::uint16_t>(s.size());
    p[0] = static_cast<char>(n >> 8);
    p[1] = static_cast<char>(n);
    p += 2;
    std::memcpy(p, s.data(), s.size());
    return p + s.size();
}

// =============================================================================
// Binary Reader Class
// =============================================================================

class BinaryReader {
   public:
    explicit BinaryReader(std::string_view data) : data_(data) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }

    std::uint16_t be16() {
        auto s = take(2);
        return static_cast<std::uint16_t>(
            (static_cast<std::uint8_t>(s[0]) << 8) |
            static_cast<std::uint8_t>(s[1]));
    }

    std::uint32_t be32() {
        auto s = take(4);
        return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0]))
                << 24) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1]))
                << 16) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2]))
                << 8) |
               static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]));
    }

    std::uint64_t be64() {
        auto s = take(8);
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | static_cast<std::uint8_t>(s[i]);
        }
        return v;
    }

    double f64() {
        std::uint64_t bits = be64();
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    std::string_view blob() {
        auto len = be32();
        return take(len);
    }

    std::string_view str() {
        auto len = be16();
        return take(len);
    }

    std::uint64_t varint() {
        std::uint64_t v = 0;
        unsigned shift = 0;
        while (off_ < data_.size()) {
            auto b = static_cast<std::uint8_t>(data_[off_++]);
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
            shift += 7;
        }
        throw std::runtime_error("binary_codec: truncated varint");
    }

    bool has_remaining() const { return off_ < data_.size(); }

    std::string_view remaining() const { return data_.substr(off_); }

    std::size_t offset() const { return off_; }

   private:
    std::string_view take(std::size_t n) {
        if (off_ + n > data_.size()) {
            throw std::runtime_error("binary_codec: truncated data");
        }
        auto s = data_.substr(off_, n);
        off_ += n;
        return s;
    }

    std::string_view data_;
    std::size_t off_ = 0;
};

}  // namespace dftracer::utils::utilities::common::serialization

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_SERIALIZATION_BINARY_CODEC_H
