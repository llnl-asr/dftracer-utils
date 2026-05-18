#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>

#include <algorithm>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal::encoding {

namespace {
namespace rocks = dftracer::utils::rocksdb;
}  // namespace

std::string prefix_for_file(int file_id) {
    return rocks::KeyCodec::encode_be32(static_cast<std::uint32_t>(file_id));
}

std::string metadata_key(int file_id) { return prefix_for_file(file_id); }

std::string checkpoint_key(int file_id, std::uint64_t uc_offset,
                           std::uint64_t checkpoint_idx) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, uc_offset);
    append_u64(key, checkpoint_idx);
    return key;
}

std::string manifest_event_key(int file_id, std::uint64_t checkpoint_idx,
                               std::string_view cat, std::string_view name) {
    std::string key("E|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, checkpoint_idx);
    key.append(cat);
    key.push_back('\0');
    key.append(name);
    return key;
}

std::string manifest_metadata_key(int file_id, std::uint64_t checkpoint_idx,
                                  std::string_view meta_type) {
    std::string key("M|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, checkpoint_idx);
    key.append(meta_type);
    return key;
}

std::string encode_metadata_record(std::uint64_t checkpoint_size,
                                   std::uint64_t total_lines,
                                   std::uint64_t total_uc_size) {
    std::string value;
    append_u64(value, checkpoint_size);
    append_u64(value, total_lines);
    append_u64(value, total_uc_size);
    return value;
}

std::string encode_checkpoint_value(const IndexerCheckpoint& checkpoint) {
    std::string value;
    append_u64(value, checkpoint.uc_size);
    append_u64(value, checkpoint.c_offset);
    append_u64(value, checkpoint.c_size);
    append_i64(value, checkpoint.bits);
    append_blob(value, checkpoint.dict_compressed);
    append_u64(value, checkpoint.num_lines);
    append_u64(value, checkpoint.first_line_num);
    append_u64(value, checkpoint.last_line_num);
    return value;
}

namespace {

// Packs lines directly into `out` as the `blob` payload of append_blob's
// wire format: u32 byte-length followed by raw little-endian uint32s.
void append_line_numbers_blob(std::string& out,
                              std::span<const std::uint32_t> lines) {
    const auto bytes =
        static_cast<std::uint32_t>(lines.size() * sizeof(std::uint32_t));
    rocks::KeyCodec::append_be32(out, bytes);
    if (!lines.empty()) {
        out.append(reinterpret_cast<const char*>(lines.data()), bytes);
    }
}

}  // namespace

std::string encode_event_range_value(std::span<const std::uint32_t> lines) {
    std::string value;
    value.reserve(sizeof(std::uint64_t) + sizeof(std::uint32_t) +
                  lines.size() * sizeof(std::uint32_t));
    append_u64(value, lines.size());
    append_line_numbers_blob(value, lines);
    return value;
}

std::string encode_metadata_value(std::span<const std::uint32_t> lines) {
    std::string value;
    value.reserve(sizeof(std::uint32_t) + lines.size() * sizeof(std::uint32_t));
    append_line_numbers_blob(value, lines);
    return value;
}

std::string file_pids_key(int file_id) {
    std::string key("P|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    return key;
}

std::string make_dimension_key(int file_id, std::string_view dimension) {
    std::string key("d|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    key.append(dimension);
    return key;
}

std::string chunk_bloom_key(int file_id, std::string_view dimension,
                            std::uint64_t checkpoint_idx) {
    std::string key = prefix_for_file(file_id);
    key.append(dimension);
    key.push_back('\0');
    append_u64(key, checkpoint_idx);
    return key;
}

std::string file_bloom_key(int file_id, std::string_view dimension) {
    std::string key = prefix_for_file(file_id);
    key.append(dimension);
    return key;
}

std::string chunk_stats_key(int file_id, std::uint64_t checkpoint_idx) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, checkpoint_idx);
    return key;
}

std::string file_scalar_stats_key(int file_id) {
    return prefix_for_file(file_id);
}

std::string file_category_counts_key(int file_id) {
    return prefix_for_file(file_id);
}

std::string file_pid_tid_counts_key(int file_id) {
    return prefix_for_file(file_id);
}

std::string file_name_counts_key(int file_id) {
    return prefix_for_file(file_id);
}

std::string chunk_dim_stats_key(int file_id, std::uint64_t checkpoint_idx,
                                std::string_view dimension) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, checkpoint_idx);
    key.append(dimension);
    return key;
}

std::string encode_bloom_value(std::span<const unsigned char> blob,
                               std::uint64_t num_entries) {
    std::string value;
    append_u64(value, num_entries);
    value.append(reinterpret_cast<const char*>(blob.data()), blob.size());
    return value;
}

std::string encode_chunk_statistics_value(
    const composites::dft::indexing::ChunkStatistics& stats) {
    std::string value;
    append_u64(value, stats.total_events);
    append_u64(value, stats.min_timestamp_us);
    append_u64(value, stats.max_timestamp_us);
    append_i64(value, stats.duration_sum_us);
    append_u64(value, stats.duration_min_us);
    append_u64(value, stats.duration_max_us);
    append_u64(value, stats.duration_count);
    append_double(value, stats.duration_m2);

    auto duration_sketch = stats.duration_sketch.serialize();
    append_blob(value, duration_sketch);

    auto duration_histogram = stats.duration_histogram.to_json();
    append_string(value, duration_histogram);

    auto name_sketches = stats.serialize_name_duration_sketches();
    append_blob(value, name_sketches);
    append_string(value, stats.name_duration_histograms_json());
    append_string(value, stats.name_duration_sums_json());
    append_string(value, stats.name_duration_sum_sqs_json());
    append_string(value, stats.name_category_json());

    auto ts_hist = stats.timestamp_histogram.serialize();
    append_blob(value, ts_hist);

    return value;
}

std::string encode_chunk_dimension_stats_value(
    const composites::dft::indexing::ChunkDimensionStats& stats,
    std::size_t value_counts_cap) {
    std::string value;
    append_u64(value, stats.distinct_count);
    append_string(value, stats.min_value);
    append_string(value, stats.max_value);
    append_string(value, stats.value_type);
    auto compressed = stats.compress_value_counts(value_counts_cap);
    append_u8(value, compressed.has_value() ? 1 : 0);
    if (compressed) {
        append_blob(value, *compressed);
    }
    return value;
}

std::string name_lookup_key(std::string_view name) {
    std::string key("s|");
    key.append(name);
    return key;
}

std::string name_reverse_key(std::uint64_t name_id) {
    std::string key("i|");
    append_u64(key, name_id);
    return key;
}

std::string name_file_posting_key(std::uint64_t name_id, int file_id) {
    std::string key("n|");
    append_u64(key, name_id);
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    return key;
}

std::string name_file_owner_key(int file_id, std::uint64_t name_id) {
    std::string key("o|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, name_id);
    return key;
}

std::string name_file_owner_prefix(int file_id) {
    std::string key("o|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    return key;
}

std::string name_chunk_posting_key(std::uint64_t name_id, int file_id,
                                   std::uint64_t checkpoint_idx) {
    std::string key("n|");
    append_u64(key, name_id);
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, checkpoint_idx);
    return key;
}

std::string name_chunk_owner_key(int file_id, std::uint64_t name_id,
                                 std::uint64_t checkpoint_idx) {
    std::string key("o|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, name_id);
    append_u64(key, checkpoint_idx);
    return key;
}

std::string name_chunk_owner_prefix(int file_id) {
    std::string key("o|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    return key;
}

std::string hash_table_forward_key(std::uint8_t type, std::string_view hash) {
    std::string key;
    key.reserve(1 + hash.size());
    key.push_back(static_cast<char>(type));
    key.append(hash);
    return key;
}

std::string hash_table_reverse_key(std::uint8_t type, std::string_view name) {
    std::string key;
    key.reserve(1 + name.size());
    key.push_back(static_cast<char>(type + 4));
    key.append(name);
    return key;
}

std::string encode_file_pids_value(
    const std::unordered_set<std::uint64_t>& pids) {
    std::vector<std::uint64_t> sorted_pids(pids.begin(), pids.end());
    std::sort(sorted_pids.begin(), sorted_pids.end());

    std::string value;
    auto encode_varint = [&value](std::uint64_t v) {
        while (v >= 0x80) {
            value.push_back(static_cast<char>(v | 0x80));
            v >>= 7;
        }
        value.push_back(static_cast<char>(v));
    };

    encode_varint(sorted_pids.size());
    for (auto pid : sorted_pids) {
        encode_varint(pid);
    }
    return value;
}

}  // namespace dftracer::utils::utilities::indexer::internal::encoding
