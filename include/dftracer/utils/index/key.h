#ifndef DFTRACER_UTILS_INDEX_KEY_H
#define DFTRACER_UTILS_INDEX_KEY_H

#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::index {

class KeyBuilder {
   public:
    KeyBuilder& u8(std::uint8_t v) {
        bytes_.push_back(v);
        return *this;
    }

    KeyBuilder& u16(std::uint16_t v) {
        append_u16_be(bytes_, v);
        return *this;
    }

    KeyBuilder& u32(std::uint32_t v) {
        append_u32_be(bytes_, v);
        return *this;
    }

    KeyBuilder& u64(std::uint64_t v) {
        append_u64_be(bytes_, v);
        return *this;
    }

    KeyBuilder& i8(std::int8_t v) {
        bytes_.push_back(static_cast<std::uint8_t>(v) ^ 0x80U);
        return *this;
    }

    KeyBuilder& i16(std::int16_t v) {
        append_u16_be(bytes_, static_cast<std::uint16_t>(
                                  static_cast<std::uint16_t>(v) ^ 0x8000U));
        return *this;
    }

    KeyBuilder& i32(std::int32_t v) {
        append_u32_be(bytes_, static_cast<std::uint32_t>(v) ^ 0x80000000U);
        return *this;
    }

    KeyBuilder& i64(std::int64_t v) {
        const auto bits = std::bit_cast<std::uint64_t>(v);
        append_u64_be(bytes_, bits ^ (std::uint64_t{1} << 63));
        return *this;
    }

    // Negative values have every IEEE bit flipped; non-negative values have
    // only the sign bit flipped. -0.0 is written as +0.0, and NaN is
    // canonicalized and sorts last.
    KeyBuilder& f32(float v);
    KeyBuilder& f64(double v);

    // A 0x00 byte becomes 0x00 0xFF and the terminator is 0x00 0x01, so
    // shorter strings sort first and embedded NULs keep bytewise order.
    KeyBuilder& str(dstd::StringView v);

    // raw must be the final part of the key.
    KeyBuilder& raw(dftracer::utils::ByteView v);

    dftracer::utils::ByteView view() const {
        return {reinterpret_cast<const std::byte*>(bytes_.data()),
                bytes_.size()};
    }

    std::size_t size() const { return bytes_.size(); }

    void clear() { bytes_.clear(); }

   private:
    dstd::Vector<std::uint8_t> bytes_;
};

class KeyReader {
   public:
    explicit KeyReader(dftracer::utils::ByteView data) : data_(data) {}

    dstd::Expected<std::uint8_t, dstd::Error> u8() {
        if (remaining() < 1) return parse_error();
        const auto value = *position();
        pos_ += 1;
        return value;
    }

    dstd::Expected<std::uint16_t, dstd::Error> u16() {
        return read_be<std::uint16_t>();
    }

    dstd::Expected<std::uint32_t, dstd::Error> u32() {
        return read_be<std::uint32_t>();
    }

    dstd::Expected<std::uint64_t, dstd::Error> u64() {
        return read_be<std::uint64_t>();
    }

    dstd::Expected<std::int8_t, dstd::Error> i8() {
        auto bits = u8();
        if (!bits) return parse_error();
        return static_cast<std::int8_t>(*bits ^ 0x80U);
    }

    dstd::Expected<std::int16_t, dstd::Error> i16() {
        auto bits = read_be<std::uint16_t>();
        if (!bits) return parse_error();
        return static_cast<std::int16_t>(*bits ^ 0x8000U);
    }

    dstd::Expected<std::int32_t, dstd::Error> i32() {
        auto bits = read_be<std::uint32_t>();
        if (!bits) return parse_error();
        return static_cast<std::int32_t>(*bits ^ 0x80000000U);
    }

    dstd::Expected<std::int64_t, dstd::Error> i64() {
        auto bits = read_be<std::uint64_t>();
        if (!bits) return parse_error();
        return std::bit_cast<std::int64_t>(*bits ^ (std::uint64_t{1} << 63));
    }

    dstd::Expected<float, dstd::Error> f32();
    dstd::Expected<double, dstd::Error> f64();

    dstd::Expected<dstd::String, dstd::Error> str();

    dftracer::utils::ByteView rest() {
        const auto out = data_.subspan(pos_);
        pos_ = data_.size();
        return out;
    }

    bool done() const { return pos_ == data_.size(); }

   private:
    template <typename T>
    dstd::Expected<T, dstd::Error> read_be() {
        if (remaining() < sizeof(T)) return parse_error();
        T value;
        std::memcpy(&value, position(), sizeof(T));
        if constexpr (std::endian::native == std::endian::little) {
            value = byte_swap(value);
        }
        pos_ += sizeof(T);
        return value;
    }

    const std::uint8_t* position() const {
        return reinterpret_cast<const std::uint8_t*>(data_.data()) + pos_;
    }

    std::size_t remaining() const { return data_.size() - pos_; }

    static dstd::Unexpected<dstd::Error> parse_error();

    dftracer::utils::ByteView data_;
    std::size_t pos_ = 0;
};

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_KEY_H
