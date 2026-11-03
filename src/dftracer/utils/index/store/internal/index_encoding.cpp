// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>

namespace dftracer::utils::index::store::internal::encoding {

dstd::String encode_gzip_member_value(
    const index::gzip::GzipMemberRecord& member) {
    dstd::String value;
    dstd::append_u64_be(value, member.c_offset);
    dstd::append_u64_be(value, member.c_size);
    dstd::append_u64_be(value, member.uc_offset);
    dstd::append_u64_be(value, member.uc_size);
    dstd::append_u64_be(value, member.first_line_num);
    dstd::append_u64_be(value, member.last_line_num);
    if (member.kind != index::gzip::GzipRecordKind::Member) {
        value.push_back(static_cast<char>(member.kind));
        value.push_back(static_cast<char>(member.bits));
    }
    return value;
}

dstd::String encode_metadata_record(std::uint64_t checkpoint_size,
                                    std::uint64_t total_lines,
                                    std::uint64_t total_uc_size,
                                    std::uint64_t flags) {
    dstd::String value;
    dstd::append_u64_be(value, checkpoint_size);
    dstd::append_u64_be(value, total_lines);
    dstd::append_u64_be(value, total_uc_size);
    dstd::append_u64_be(value, flags);
    return value;
}

dstd::String encode_chunk_statistics_value(
    const index::schemas::dft::ChunkStatistics& stats) {
    dstd::String value;
    dstd::append_u64_be(value, stats.total_events);
    dstd::append_u64_be(value, stats.min_timestamp_us);
    dstd::append_u64_be(value, stats.max_timestamp_us);
    dstd::append_u64_be(value,
                        static_cast<std::uint64_t>(stats.duration_sum_us));
    dstd::append_u64_be(value, stats.duration_min_us);
    dstd::append_u64_be(value, stats.duration_max_us);
    dstd::append_u64_be(value, stats.duration_count);
    dstd::append_f64_be(value, stats.duration_m2);

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
