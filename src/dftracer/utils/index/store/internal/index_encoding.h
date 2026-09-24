#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_ENCODING_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_ENCODING_H

#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>
#include <dftracer/utils/index/store/key_codec.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dftracer::utils::index::store::internal::encoding {

// Payload encoders of the core.members and dft.index records; keys and value
// headers come from store/layout.h.

std::string encode_gzip_member_value(
    const index::gzip::GzipMemberRecord& member);

/// Bits of the metadata record's flags word.
inline constexpr std::uint64_t METADATA_FLAG_TRUNCATED = 1;

std::string encode_metadata_record(std::uint64_t checkpoint_size,
                                   std::uint64_t total_lines,
                                   std::uint64_t total_uc_size,
                                   std::uint64_t flags);

std::string encode_chunk_statistics_value(
    const index::schemas::dft::ChunkStatistics& stats);

// Accepts any map type exposing string keys and uint64 values.
template <typename Map>
std::string encode_count_map_value(const Map& counts) {
    std::string value;
    value.reserve(sizeof(std::uint32_t) +
                  counts.size() *
                      (sizeof(std::uint32_t) + sizeof(std::uint64_t)));
    index::store::KeyCodec::append_be32(
        value, static_cast<std::uint32_t>(counts.size()));
    for (const auto& [key, count] : counts) {
        append_string(value, key);
        append_u64(value, count);
    }
    return value;
}

}  // namespace dftracer::utils::index::store::internal::encoding

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_INDEX_ENCODING_H
