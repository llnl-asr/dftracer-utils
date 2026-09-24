#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>

namespace dftracer::utils::index::store::internal::encoding {

std::string encode_gzip_member_value(
    const index::gzip::GzipMemberRecord& member) {
    std::string value;
    append_u64(value, member.c_offset);
    append_u64(value, member.c_size);
    append_u64(value, member.uc_offset);
    append_u64(value, member.uc_size);
    append_u64(value, member.first_line_num);
    append_u64(value, member.last_line_num);
    return value;
}

std::string encode_metadata_record(std::uint64_t checkpoint_size,
                                   std::uint64_t total_lines,
                                   std::uint64_t total_uc_size,
                                   std::uint64_t flags) {
    std::string value;
    append_u64(value, checkpoint_size);
    append_u64(value, total_lines);
    append_u64(value, total_uc_size);
    append_u64(value, flags);
    return value;
}

std::string encode_chunk_statistics_value(
    const index::schemas::dft::ChunkStatistics& stats) {
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

    // Per-cat and per-pid duration aggregates.
    append_blob(value,
                index::schemas::dft::ChunkStatistics::serialize_sketch_map(
                    stats.cat_duration_sketches));
    append_string(value, stats.cat_duration_sums_json());
    append_blob(value,
                index::schemas::dft::ChunkStatistics::serialize_sketch_map(
                    stats.pid_duration_sketches));
    append_string(value, stats.pid_duration_sums_json());

    return value;
}

}  // namespace dftracer::utils::index::store::internal::encoding
