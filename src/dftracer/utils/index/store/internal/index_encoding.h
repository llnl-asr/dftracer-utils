// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_ENCODING_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_ENCODING_H

#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::internal::encoding {

// Payload encoders of the core.members and dft.index records. A value is the
// payload alone and its key comes from index/tables.

dstd::String encode_gzip_member_value(
    const index::gzip::GzipMemberRecord& member);

/// Bits of the metadata record's flags word.
inline constexpr std::uint64_t METADATA_FLAG_TRUNCATED = 1;
inline constexpr std::uint64_t METADATA_FLAG_OPEN_LINE = 2;
inline constexpr std::uint64_t METADATA_FLAG_MID_LINE_CHUNK = 4;
// A restart piece has at most 7 spare bits, so the top bit of the byte is free.
inline constexpr std::uint8_t MEMBER_FLAG_OPENS_MID_LINE = 0x80;

dstd::String encode_metadata_record(std::uint64_t checkpoint_size,
                                    std::uint64_t total_lines,
                                    std::uint64_t total_uc_size,
                                    std::uint64_t flags,
                                    std::uint64_t next_member_idx);

dstd::String encode_chunk_statistics_value(
    const index::schemas::dft::ChunkStatistics& stats);

// Accepts any map type exposing string keys and uint64 values.
template <typename Map>
dstd::String encode_count_map_value(const Map& counts) {
    dstd::String value;
    value.reserve(sizeof(std::uint32_t) +
                  counts.size() *
                      (sizeof(std::uint32_t) + sizeof(std::uint64_t)));
    dstd::append_u32_be(value, static_cast<std::uint32_t>(counts.size()));
    for (const auto& [key, count] : counts) {
        append_string(value, key);
        dstd::append_u64_be(value, count);
    }
    return value;
}

}  // namespace dftracer::utils::index::store::internal::encoding

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_ENCODING_H
