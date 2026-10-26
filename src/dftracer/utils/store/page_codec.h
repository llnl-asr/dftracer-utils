#ifndef DFTRACER_UTILS_STORE_PAGE_CODEC_H
#define DFTRACER_UTILS_STORE_PAGE_CODEC_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store {

/// A stored page starts with this header: u8 codec, three zero bytes, u32 raw
/// length, u32 payload length, four zero bytes. The payload follows, then zero
/// padding. Section offsets count from the start of the raw bytes.
inline constexpr std::size_t PAGE_HEADER_BYTES = 16;

/// The most raw bytes one stored page may claim, so a page that passes its crc
/// but was written wrong cannot ask for an unbounded buffer.
inline constexpr std::uint32_t PAGE_MAX_RAW_BYTES = 1U << 30U;

enum class PageCodec : std::uint8_t { None = 0, Zstd = 1 };

/// The stored form of `raw`: the header, then `raw` compressed with zstd when
/// that saves at least an eighth of it and `raw` itself otherwise, padded with
/// zeros to a multiple of `align`.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_page(
    ByteView raw, std::size_t align);

/// The raw bytes of a stored page; a Parse error when the header or the
/// payload does not decode. `stored` is reused as the result.
dstd::Expected<void, dstd::Error> decode_page(
    dstd::Vector<std::uint8_t>& stored);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_PAGE_CODEC_H
