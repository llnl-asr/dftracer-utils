// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_DECODING_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_DECODING_H

#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/file.h>

#include <cstdint>
#include <string_view>

namespace dftracer::utils::index::store::internal::decoding {

// Payload decoders for the records index_encoding.h writes. They throw on a
// malformed payload.

index::schemas::dft::ChunkStatistics decode_chunk_statistics_value(
    std::string_view value);

index::gzip::GzipMemberRecord decode_gzip_member(std::uint32_t member_idx,
                                                 std::string_view body);

dstd::StringViewMap<std::uint64_t> decode_count_map_value(
    std::string_view value);

FileMetadataResult decode_metadata_record(std::string_view value);

}  // namespace dftracer::utils::index::store::internal::decoding

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_DECODING_H
