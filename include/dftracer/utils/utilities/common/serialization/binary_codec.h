#ifndef DFTRACER_UTILS_UTILITIES_COMMON_SERIALIZATION_BINARY_CODEC_H
#define DFTRACER_UTILS_UTILITIES_COMMON_SERIALIZATION_BINARY_CODEC_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

// Length-prefixed framing and a throwing reader on top of the dstd codec in
// dstd/big_endian.h. The integers, doubles and varints are written with
// append_u*_be, append_f64_be and append_varint from there.
namespace dftracer::utils::utilities::common::serialization {

/// u16 length, then the bytes.
inline void put_str(std::string& out, std::string_view s) {
    append_u16_be(out, static_cast<std::uint16_t>(s.size()));
    out.append(s.data(), s.size());
}

/// u32 length, then the bytes.
inline void put_blob(std::string& out, std::span<const std::uint8_t> data) {
    append_u32_be(out, static_cast<std::uint32_t>(data.size()));
    out.append(reinterpret_cast<const char*>(data.data()), data.size());
}

/// Writes a put_str frame at p, which has room for it, and returns the end.
inline std::uint8_t* write_str(std::uint8_t* p, std::string_view s) {
    write_u16_be(p, static_cast<std::uint16_t>(s.size()));
    p += 2;
    std::memcpy(p, s.data(), s.size());
    return p + s.size();
}

/// Cursor over a byte string. Every read that does not fit throws
/// DFTUtilsException(ErrorCode::Parse).
class BinaryReader {
   public:
    explicit BinaryReader(std::string_view data) : data_(data) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }

    std::uint16_t be16() { return read_u16_be(bytes(take(2))); }

    std::uint32_t be32() { return read_u32_be(bytes(take(4))); }

    std::uint64_t be64() { return read_u64_be(bytes(take(8))); }

    double f64() { return read_f64_be(bytes(take(8))); }

    std::string_view blob() {
        auto len = be32();
        return take(len);
    }

    void skip(std::size_t n) { take(n); }

    void skip_blob() {
        auto len = be32();
        take(len);
    }

    std::string_view str() {
        auto len = be16();
        return take(len);
    }

    std::uint64_t varint() {
        const auto* p = bytes(data_.substr(off_));
        const auto* end = p + (data_.size() - off_);
        std::uint64_t v = 0;
        if (!read_varint(p, end, v))
            throw DFTUtilsException(ErrorCode::Parse,
                                    "binary_codec: truncated varint");
        off_ = static_cast<std::size_t>(
            p - reinterpret_cast<const std::uint8_t*>(data_.data()));
        return v;
    }

    bool has_remaining() const { return off_ < data_.size(); }

    std::string_view remaining() const { return data_.substr(off_); }

    std::size_t offset() const { return off_; }

   private:
    static const std::uint8_t* bytes(std::string_view s) {
        return reinterpret_cast<const std::uint8_t*>(s.data());
    }

    std::string_view take(std::size_t n) {
        if (off_ + n > data_.size()) {
            throw DFTUtilsException(ErrorCode::Parse,
                                    "binary_codec: truncated data");
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
