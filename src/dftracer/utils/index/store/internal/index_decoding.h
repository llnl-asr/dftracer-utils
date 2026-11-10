// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_DECODING_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_DECODING_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/file.h>

#include <cstdint>
#include <string_view>

namespace dftracer::utils::index::store::internal::decoding {

// Payload decoders for the records index_encoding.h writes. They return an
// Indexer error on a malformed payload.

dstd::Result<index::schemas::dft::ChunkStatistics>
decode_chunk_statistics_value(dstd::StringView value);

dstd::Result<index::gzip::GzipMemberRecord> decode_gzip_member(
    std::uint32_t member_idx, dstd::StringView body);

dstd::Result<dstd::StringViewMap<std::uint64_t>> decode_count_map_value(
    dstd::StringView value);

dstd::Result<FileMetadataResult> decode_metadata_record(dstd::StringView value);

}  // namespace dftracer::utils::index::store::internal::decoding

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_DECODING_H
