#include <dftracer/utils/index/key.h>

#include <bit>
#include <cmath>

namespace dftracer::utils::index {

KeyBuilder& KeyBuilder::f32(float v) {
    std::uint32_t bits = 0;
    if (std::isnan(v)) {
        bits = 0x7FC00000U;
    } else if (v != 0.0F) {
        bits = std::bit_cast<std::uint32_t>(v);
    }
    const std::uint32_t sign_bit = 0x80000000U;
    bits = (bits & sign_bit) == 0 ? bits ^ sign_bit : ~bits;
    append_u32_be(bytes_, bits);
    return *this;
}

KeyBuilder& KeyBuilder::f64(double v) {
    std::uint64_t bits = 0;
    if (std::isnan(v)) {
        bits = std::uint64_t{0x7FF8} << 48;
    } else if (v != 0.0) {
        bits = std::bit_cast<std::uint64_t>(v);
    }
    const auto sign_bit = std::uint64_t{1} << 63;
    if ((bits & sign_bit) == 0) {
        bits ^= sign_bit;
    } else {
        bits = ~bits;
    }
    append_u64_be(bytes_, bits);
    return *this;
}

KeyBuilder& KeyBuilder::str(dstd::StringView v) {
    for (const char c : v) {
        const auto byte = static_cast<std::uint8_t>(c);
        bytes_.push_back(byte);
        if (byte == 0) bytes_.push_back(0xFF);
    }
    bytes_.push_back(0);
    bytes_.push_back(1);
    return *this;
}

KeyBuilder& KeyBuilder::raw(dftracer::utils::ByteView v) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(v.data());
    bytes_.insert(bytes_.end(), begin, begin + v.size());
    return *this;
}

dstd::Expected<float, dstd::Error> KeyReader::f32() {
    auto bits = read_be<std::uint32_t>();
    if (!bits) return parse_error();
    const std::uint32_t sign_bit = 0x80000000U;
    const auto decoded = (*bits & sign_bit) == 0 ? ~*bits : *bits ^ sign_bit;
    return std::bit_cast<float>(decoded);
}

dstd::Expected<double, dstd::Error> KeyReader::f64() {
    auto bits = read_be<std::uint64_t>();
    if (!bits) return parse_error();
    const auto sign_bit = std::uint64_t{1} << 63;
    const auto decoded = (*bits & sign_bit) == 0 ? ~*bits : *bits ^ sign_bit;
    return std::bit_cast<double>(decoded);
}

dstd::Expected<dstd::String, dstd::Error> KeyReader::str() {
    const std::size_t start = pos_;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data_.data());
    dstd::String out;
    std::size_t at = start;
    while (at + 1 < data_.size()) {
        const std::uint8_t byte = bytes[at];
        if (byte != 0) {
            out.push_back(static_cast<char>(byte));
            ++at;
            continue;
        }
        const std::uint8_t next = bytes[at + 1];
        if (next == 1) {
            pos_ = at + 2;
            return out;
        }
        if (next != 0xFF) return parse_error();
        out.push_back('\0');
        at += 2;
    }
    return parse_error();
}

dstd::Unexpected<dstd::Error> KeyReader::parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

}  // namespace dftracer::utils::index
