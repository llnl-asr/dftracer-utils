// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_VARINT_H
#define DFTRACER_UTILS_STORE_VARINT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/varint.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store::engine {

inline void encode_varint(dstd::Vector<std::uint8_t>& out,
                          std::uint64_t value) {
    dstd::append_varint(out, value);
}

/// Decodes one varint at offset and moves offset past it. Truncated or
/// over-long input returns Parse and leaves offset where it was.
inline dstd::Expected<std::uint64_t, dstd::Error> decode_varint(
    dftracer::utils::dstd::ByteView bytes, std::size_t& offset) {
    const std::size_t start = offset;
    std::uint64_t value = 0;
    if (start < bytes.size()) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const std::uint8_t* p = begin + start;
        if (dstd::read_varint(p, begin + bytes.size(), value)) {
            offset = static_cast<std::size_t>(p - begin);
            return value;
        }
    }
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_VARINT_H
