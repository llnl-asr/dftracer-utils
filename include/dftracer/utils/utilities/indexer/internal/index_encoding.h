#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INDEX_ENCODING_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INDEX_ENCODING_H

#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace dftracer::utils::utilities::indexer::internal::encoding {

std::string prefix_for_file(int file_id);

/// DEFAULT-CF key holding the monotonically increasing counter for the next
/// file_id to assign. Used by both `get_or_create_file_info` (single-file
/// path) and `IndexDatabase::reserve_file_id_range` (distributed pre-alloc).
inline constexpr std::string_view NEXT_FILE_ID_KEY = "_next_file_id";

std::string metadata_key(int file_id);

std::string checkpoint_key(int file_id, std::uint64_t uc_offset,
                           std::uint64_t checkpoint_idx);

std::string manifest_event_key(int file_id, std::uint64_t checkpoint_idx,
                               std::string_view cat, std::string_view name);

std::string manifest_metadata_key(int file_id, std::uint64_t checkpoint_idx,
                                  std::string_view meta_type);

std::string encode_metadata_record(std::uint64_t checkpoint_size,
                                   std::uint64_t total_lines,
                                   std::uint64_t total_uc_size);

std::string encode_checkpoint_value(const IndexerCheckpoint& checkpoint);

std::string encode_event_range_value(std::span<const std::uint32_t> lines);

std::string encode_metadata_value(std::span<const std::uint32_t> lines);

std::string file_pids_key(int file_id);

std::string encode_file_pids_value(
    const std::unordered_set<std::uint64_t>& pids);

// Bloom / stats / dimension CFs --------------------------------------------

std::string make_dimension_key(int file_id, std::string_view dimension);

std::string chunk_bloom_key(int file_id, std::string_view dimension,
                            std::uint64_t checkpoint_idx);

std::string file_bloom_key(int file_id, std::string_view dimension);

std::string chunk_stats_key(int file_id, std::uint64_t checkpoint_idx);

std::string file_scalar_stats_key(int file_id);
std::string file_category_counts_key(int file_id);
std::string file_pid_tid_counts_key(int file_id);
std::string file_name_counts_key(int file_id);

std::string chunk_dim_stats_key(int file_id, std::uint64_t checkpoint_idx,
                                std::string_view dimension);

// Name dictionary + postings (name_id is a 64-bit FNV1a hash of the name) ---

std::string name_lookup_key(std::string_view name);
std::string name_reverse_key(std::uint64_t name_id);

std::string name_file_posting_key(std::uint64_t name_id, int file_id);
std::string name_file_owner_key(int file_id, std::uint64_t name_id);
std::string name_file_owner_prefix(int file_id);

std::string name_chunk_posting_key(std::uint64_t name_id, int file_id,
                                   std::uint64_t checkpoint_idx);
std::string name_chunk_owner_key(int file_id, std::uint64_t name_id,
                                 std::uint64_t checkpoint_idx);
std::string name_chunk_owner_prefix(int file_id);

// Hash tables (content-addressed) ------------------------------------------

std::string hash_table_forward_key(std::uint8_t type, std::string_view hash);
std::string hash_table_reverse_key(std::uint8_t type, std::string_view name);

std::string encode_bloom_value(std::span<const unsigned char> blob,
                               std::uint64_t num_entries);

std::string encode_chunk_statistics_value(
    const composites::dft::indexing::ChunkStatistics& stats);

std::string encode_chunk_dimension_stats_value(
    const composites::dft::indexing::ChunkDimensionStats& stats,
    std::size_t value_counts_cap);

// Count map and name summary encoders are templated so they can accept any
// map type exposing string keys and uint64 values.
template <typename Map>
std::string encode_count_map_value(const Map& counts) {
    std::string value;
    value.reserve(sizeof(std::uint32_t) +
                  counts.size() *
                      (sizeof(std::uint32_t) + sizeof(std::uint64_t)));
    dftracer::utils::rocksdb::KeyCodec::append_be32(
        value, static_cast<std::uint32_t>(counts.size()));
    for (const auto& [key, count] : counts) {
        append_string(value, key);
        append_u64(value, count);
    }
    return value;
}

template <typename Map>
std::string encode_name_summary_value(const Map& counts,
                                      std::uint64_t other_count,
                                      std::uint64_t unique_count) {
    std::string value;
    value.reserve(sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t) +
                  counts.size() *
                      (sizeof(std::uint32_t) + sizeof(std::uint64_t)));
    dftracer::utils::rocksdb::KeyCodec::append_be32(
        value, static_cast<std::uint32_t>(counts.size()));
    append_u64(value, other_count);
    append_u64(value, unique_count);
    for (const auto& [key, count] : counts) {
        append_string(value, key);
        append_u64(value, count);
    }
    return value;
}

}  // namespace dftracer::utils::utilities::indexer::internal::encoding

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INDEX_ENCODING_H
