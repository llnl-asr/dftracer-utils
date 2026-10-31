#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_VARINT_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_VARINT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::engine {

inline constexpr std::size_t VARINT_MAX_BYTES = 10;

inline void encode_varint(dstd::Vector<std::uint8_t>& out,
                          std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

inline dstd::Expected<std::uint64_t, dstd::Error> decode_varint(
    dftracer::utils::ByteView bytes, std::size_t& offset) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t start = offset;
    std::uint64_t value = 0;
    unsigned shift = 0;
    for (std::size_t count = 0; count < VARINT_MAX_BYTES; ++count) {
        if (offset >= bytes.size()) {
            offset = start;
            return dstd::Unexpected<dstd::Error>(
                dstd::Error(dstd::Condition::Parse));
        }
        const std::uint8_t byte = data[offset++];
        if (shift == 63U && byte > 1U) {
            offset = start;
            return dstd::Unexpected<dstd::Error>(
                dstd::Error(dstd::Condition::Parse));
        }
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7U;
    }
    offset = start;
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_VARINT_H
