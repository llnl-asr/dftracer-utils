// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_SERIALIZATION_BINARY_CODEC_H
#define DFTRACER_UTILS_FILEIO_SERIALIZATION_BINARY_CODEC_H

#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::fileio::serialization {

/// u16 length, then the bytes. Appends to any byte string, as the dstd
/// append_*_be helpers do.
template <typename Buf>
inline void put_str(Buf& out, dstd::StringView s) {
    dstd::append_u16_be(out, static_cast<std::uint16_t>(s.size()));
    out.append(s.data(), s.size());
}

}  // namespace dftracer::utils::fileio::serialization

#endif  // DFTRACER_UTILS_FILEIO_SERIALIZATION_BINARY_CODEC_H
